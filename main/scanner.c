#include "scanner.h"
#include <string.h>
#include "board.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/pulse_cnt.h"
#include "soc/gpio_reg.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

static const char *TAG = "scanner";

#define N_SCAN      10          /* 9 keys + encoder switch */
#define TIMER_HZ    8000000     /* 125 ns resolution: 16 kHz = exactly 500 ticks */

/* Everything the ISR touches lives in internal RAM (DRAM_ATTR / plain statics) */
static DRAM_ATTR const uint8_t s_pins[N_SCAN] = {
    BOARD_KEY_PIN_LIST, BOARD_ENC_SW_PIN,
};
static DRAM_ATTR const uint8_t s_keys[N_SCAN] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, KEY_ENC_SW };

static QueueHandle_t s_q;
static gptimer_handle_t s_timer;
static volatile uint32_t s_stable;          /* debounced state, bit = key index */
static uint16_t s_lockout[N_SCAN];          /* remaining locked samples per input */
static volatile uint16_t s_lock_samples;    /* debounce_ms * kHz */
static volatile int s_khz;
static int s_debounce_ms;

static pcnt_unit_handle_t s_pcnt;
static pcnt_channel_handle_t s_ch_a, s_ch_b;
static volatile bool s_enc_reverse;
static int s_enc_steps;
static volatile uint32_t s_enc_cnt[2];

_Static_assert(sizeof((const uint8_t[]){ BOARD_KEY_PIN_LIST }) == NUM_BUTTONS, "BOARD_KEY_PIN_LIST must list 9 pins");

/* ---------- key scan ISR ---------- */

static bool IRAM_ATTR scan_isr(gptimer_handle_t timer, const gptimer_alarm_event_data_t *ev, void *ctx)
{
    const uint32_t lo = REG_READ(GPIO_IN_REG);   /* GPIO 0..31 */
    const uint32_t hi = REG_READ(GPIO_IN1_REG);  /* GPIO 32..48 */
    uint32_t stable = s_stable;
    BaseType_t woken = pdFALSE;

    for (int i = 0; i < N_SCAN; i++) {
        if (s_lockout[i]) {
            s_lockout[i]--;
            continue;
        }
        const uint8_t pin = s_pins[i];
        const bool pressed = !(pin < 32 ? (lo >> pin) & 1 : (hi >> (pin - 32)) & 1); /* active low */
        const uint32_t bit = 1u << s_keys[i];
        if (pressed != ((stable & bit) != 0)) {
            stable ^= bit;
            s_lockout[i] = s_lock_samples;
            scan_event_t e = { .ev = s_keys[i] | (pressed ? SCAN_EV_PRESS : 0), .t_us = (uint32_t)esp_timer_get_time() };
            xQueueSendFromISR(s_q, &e, &woken);
        }
    }
    s_stable = stable;
    return woken == pdTRUE;
}

/* ---------- encoder ISR (PCNT watch point = one detent) ---------- */

static bool IRAM_ATTR enc_isr(pcnt_unit_handle_t unit, const pcnt_watch_event_data_t *ev, void *ctx)
{
    bool cw = ev->watch_point_value < 0; /* negative count = clockwise with this A/B wiring */
    if (s_enc_reverse) cw = !cw;
    s_enc_cnt[cw ? 1 : 0]++;
    scan_event_t e = { .ev = (cw ? KEY_ENC_CW : KEY_ENC_CCW) | SCAN_EV_PRESS, .t_us = (uint32_t)esp_timer_get_time() };
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_q, &e, &woken);
    return woken == pdTRUE;
}

static void encoder_create(int steps)
{
    pcnt_unit_config_t uc = {
        .high_limit = steps,  /* the counter auto-clears when it reaches a limit */
        .low_limit = -steps,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&uc, &s_pcnt));
    pcnt_glitch_filter_config_t f = { .max_glitch_ns = 1000 };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_pcnt, &f));

    /* full quadrature (x4) decoding with two channels */
    pcnt_chan_config_t a = { .edge_gpio_num = BOARD_ENC_A_PIN, .level_gpio_num = BOARD_ENC_B_PIN };
    pcnt_chan_config_t b = { .edge_gpio_num = BOARD_ENC_B_PIN, .level_gpio_num = BOARD_ENC_A_PIN };
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &a, &s_ch_a));
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &b, &s_ch_b));
    pcnt_channel_set_edge_action(s_ch_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(s_ch_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(s_ch_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(s_ch_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    gpio_pullup_en(BOARD_ENC_A_PIN);
    gpio_pullup_en(BOARD_ENC_B_PIN);

    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(s_pcnt, steps));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(s_pcnt, -steps));
    pcnt_event_callbacks_t cbs = { .on_reach = enc_isr };
    ESP_ERROR_CHECK(pcnt_unit_register_event_callbacks(s_pcnt, &cbs, NULL));
    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt));
}

static void encoder_destroy(void)
{
    if (!s_pcnt) return;
    pcnt_unit_stop(s_pcnt);
    pcnt_unit_disable(s_pcnt);
    pcnt_del_channel(s_ch_a);
    pcnt_del_channel(s_ch_b);
    pcnt_del_unit(s_pcnt);
    s_pcnt = NULL;
}

/* ---------- public API ---------- */

static void inputs_init(void)
{
    uint64_t mask = (1ULL << BOARD_ENC_A_PIN) | (1ULL << BOARD_ENC_B_PIN);
    for (int i = 0; i < N_SCAN; i++) mask |= 1ULL << s_pins[i];
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}

bool scanner_raw_key1_pressed(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_SETUP_KEY_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    esp_rom_delay_us(2000); /* let the pull-up charge the line */
    return gpio_get_level(BOARD_SETUP_KEY_PIN) == 0;
}

static uint32_t alarm_ticks(int khz) { return TIMER_HZ / (khz * 1000); }

void scanner_start(QueueHandle_t q, int khz, int debounce_ms, int enc_steps, bool enc_reverse)
{
    s_q = q;
    inputs_init();
    esp_rom_delay_us(1000);

    /* keys already held at power-up are ignored until released (no stray key at boot) */
    const uint32_t lo = REG_READ(GPIO_IN_REG), hi = REG_READ(GPIO_IN1_REG);
    uint32_t st = 0;
    for (int i = 0; i < N_SCAN; i++) {
        uint8_t pin = s_pins[i];
        if (!(pin < 32 ? (lo >> pin) & 1 : (hi >> (pin - 32)) & 1)) st |= 1u << s_keys[i];
    }
    s_stable = st;

    s_khz = khz;
    s_debounce_ms = debounce_ms;
    s_lock_samples = debounce_ms * khz;

    gptimer_config_t tc = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = TIMER_HZ,
        .intr_priority = 3, /* highest level available to C handlers */
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&tc, &s_timer));
    gptimer_event_callbacks_t cbs = { .on_alarm = scan_isr };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(s_timer, &cbs, NULL));
    gptimer_alarm_config_t ac = {
        .alarm_count = alarm_ticks(khz),
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(s_timer, &ac));
    ESP_ERROR_CHECK(gptimer_enable(s_timer));
    ESP_ERROR_CHECK(gptimer_start(s_timer));

    s_enc_reverse = enc_reverse;
    s_enc_steps = enc_steps;
    encoder_create(enc_steps);
    ESP_LOGI(TAG, "scanning at %d kHz, debounce %d ms, encoder %d counts/detent", khz, debounce_ms, enc_steps);
}

void scanner_set_rate(int khz)
{
    if (khz < SCAN_KHZ_MIN) khz = SCAN_KHZ_MIN;
    if (khz > SCAN_KHZ_MAX) khz = SCAN_KHZ_MAX;
    while (khz & (khz - 1)) khz &= khz - 1; /* 1, 2, 4, 8, 16 */
    if (!s_timer || khz == s_khz) return;
    s_khz = khz;
    s_lock_samples = s_debounce_ms * khz;
    gptimer_alarm_config_t ac = {
        .alarm_count = alarm_ticks(khz),
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    gptimer_set_alarm_action(s_timer, &ac);
    ESP_LOGI(TAG, "scan rate %d kHz", khz);
}

void scanner_set_debounce(int debounce_ms)
{
    s_debounce_ms = debounce_ms;
    s_lock_samples = debounce_ms * s_khz;
}

void scanner_set_encoder(int steps, bool reverse)
{
    s_enc_reverse = reverse;
    /* the unit only needs rebuilding when the counts-per-detent changed */
    if (!s_pcnt || steps == s_enc_steps) return;
    s_enc_steps = steps;
    encoder_destroy();
    encoder_create(steps);
    ESP_LOGI(TAG, "encoder %d counts/detent", steps);
}

uint32_t scanner_state(void) { return s_stable; }
int scanner_rate(void) { return s_khz; }

void scanner_enc_counts(uint32_t *ccw, uint32_t *cw)
{
    *ccw = s_enc_cnt[0];
    *cw = s_enc_cnt[1];
}
