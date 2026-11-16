#include "keyproc.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "config.h"
#include "scanner.h"
#include "hid_out.h"
#include "auth.h"
#include "latency.h"
#include "class/hid/hid.h"

static const char *TAG = "keys";

#define KEYPROC_PRIO  21 /* above TinyUSB (20) and everything network related */
#define KEYPROC_CORE  1  /* Wi-Fi and BLE controller/host live on core 0 */
#define LONG_PRESS_MS 1500

typedef struct {
    bool down;
    uint32_t seq;        /* press order, oldest first in the 6KRO report */
    key_action_t act;    /* snapshot taken at press time (profile may change while held) */
} held_t;

static QueueHandle_t s_q;
static held_t s_held[NUM_KEYS];
static uint32_t s_seq;
/* encoder button */
static bool s_btn_down;              /* pressed (after boot) and not yet released */
static bool s_btn_long;              /* the current press already fired its long-press action */
static TickType_t s_btn_since;
/* profile selection mode */
static volatile bool s_selecting;
static volatile int s_pending;       /* profile shown while selecting */
static int s_sel_origin;             /* profile active when selection started (for cancel) */
static uint32_t s_swallow;           /* keys pressed in selection mode: ignore their release */
static uint8_t s_last_kb[8];
static uint16_t s_last_cc;
static uint16_t s_last_sys;

static const uint8_t s_ascii[128][2] = { HID_ASCII_TO_KEYCODE };

/* ---------- report building ---------- */

static void build_kb(uint8_t out[8])
{
    memset(out, 0, 8);
    bool used[NUM_KEYS] = { 0 };
    int n = 0;
    for (int k = 0; k < NUM_KEYS; k++) {
        if (s_held[k].down && s_held[k].act.type == ACT_KEY) out[0] |= s_held[k].act.mods;
    }
    /* keycodes in press order; duplicates collapse, extras beyond 6 are dropped (6KRO) */
    while (n < 6) {
        int best = -1;
        for (int k = 0; k < NUM_KEYS; k++) {
            const held_t *h = &s_held[k];
            if (!h->down || used[k] || h->act.type != ACT_KEY || !h->act.key) continue;
            if (best < 0 || h->seq < s_held[best].seq) best = k;
        }
        if (best < 0) break;
        used[best] = true;
        uint8_t code = s_held[best].act.key;
        if (!memchr(out + 2, code, n)) out[2 + n++] = code;
    }
}

/* consumer and system reports hold one usage: the most recently pressed key of that type wins */
static uint16_t build_usage(uint8_t type)
{
    int best = -1;
    for (int k = 0; k < NUM_KEYS; k++) {
        const held_t *h = &s_held[k];
        if (h->down && h->act.type == type && (best < 0 || h->seq > s_held[best].seq)) best = k;
    }
    return best < 0 ? 0 : s_held[best].act.usage;
}

static void flush(bool force)
{
    uint8_t kb[8];
    build_kb(kb);
    if (force || memcmp(kb, s_last_kb, 8) != 0) {
        hid_out_keyboard(kb[0], kb + 2);
        memcpy(s_last_kb, kb, 8);
    }
    uint16_t cc = build_usage(ACT_MEDIA);
    if (force || cc != s_last_cc) {
        hid_out_consumer(cc);
        s_last_cc = cc;
    }
    uint16_t sys = build_usage(ACT_SYSTEM);
    if (force || sys != s_last_sys) {
        hid_out_system(sys);
        s_last_sys = sys;
    }
}

static void type_text(const char *text)
{
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 128 || !s_ascii[c][1]) continue;
        uint8_t keys[6] = { s_ascii[c][1] };
        hid_out_keyboard(s_ascii[c][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0, keys);
        uint8_t none[6] = { 0 };
        hid_out_keyboard(0, none);
    }
    memset(s_last_kb, 0xFF, sizeof(s_last_kb)); /* resend the held state afterwards */
}

/* ---------- key handling ---------- */

static void key_down(int k, const key_action_t *a)
{
    if (a->type == ACT_TEXT) {
        type_text(a->text);
        flush(false);
        return;
    }
    s_held[k].down = true;
    s_held[k].seq = ++s_seq;
    s_held[k].act = *a;
    flush(false);
}

static void key_up(int k)
{
    if (!s_held[k].down) return;
    s_held[k].down = false;
    flush(false);
}

static void tap(int k, const key_action_t *a)
{
    key_down(k, a);
    key_up(k);
}

/* ---------- profile selection mode ----------
 * Entered by a long press of the encoder button. The encoder steps through profiles (shown on the
 * LED, not yet active); key 1..9 activates that profile and exits; a short press of the encoder
 * button activates the shown profile; another long press cancels and keeps the previous one. */

static void sel_enter(void)
{
    s_sel_origin = config_active();
    s_pending = s_sel_origin;
    s_selecting = true;
    ESP_LOGI(TAG, "profile selection: start at %d", s_pending + 1);
}

static void sel_finish(int idx)
{
    s_selecting = false;
    config_set_active(idx);
    ESP_LOGI(TAG, "profile selection: profile %d active", idx + 1);
}

static void sel_cancel(void)
{
    s_selecting = false;
    ESP_LOGI(TAG, "profile selection: cancelled, profile %d stays", s_sel_origin + 1);
}

/* encoder button held for LONG_PRESS_MS (fires while still held) */
static void on_long_press(void)
{
    s_btn_long = true;
    if (s_selecting) sel_cancel();
    else sel_enter();
}

static void on_short_press(void)
{
    if (s_selecting) {
        sel_finish(s_pending);
    } else {
        key_action_t a;
        config_get_action(KEY_ENC_SW, &a);
        tap(KEY_ENC_SW, &a);
    }
}

static void handle(uint8_t ev)
{
    const int k = SCAN_EV_KEY(ev);
    const bool press = ev & SCAN_EV_PRESS;
    key_action_t a;

    /* web UI verification pending: key presses are the one-time code and never reach the host.
     * Releases still pass, so a key held before the challenge started is released properly. */
    if (press && auth_capturing()) {
        if (k < NUM_BUTTONS) auth_key_pressed(k);
        else if (k == KEY_ENC_SW) auth_cancel(); /* physical override: drop the challenge */
        return;
    }

    if (k == KEY_ENC_SW) {
        if (press) {
            s_btn_down = true;
            s_btn_long = false;
            s_btn_since = xTaskGetTickCount();
        } else if (s_btn_down) { /* (ignores the release of a press that started before boot) */
            s_btn_down = false;
            if (!s_btn_long) on_short_press();
        }
        return;
    }

    if (k == KEY_ENC_CW || k == KEY_ENC_CCW) {
        if (s_selecting) {
            s_pending = (s_pending + (k == KEY_ENC_CW ? 1 : NUM_PROFILES - 1)) % NUM_PROFILES;
            return;
        }
        config_get_action(k, &a);
        tap(k, &a);
        return;
    }

    if (k >= NUM_BUTTONS) return;
    if (press) {
        if (s_selecting) {
            s_swallow |= 1u << k;
            sel_finish(k);
            return;
        }
        config_get_action(k, &a);
        key_down(k, &a);
    } else {
        if (s_swallow & (1u << k)) {
            s_swallow &= ~(1u << k);
            return;
        }
        key_up(k);
    }
}

static void keyproc_task(void *arg)
{
    global_cfg_t g;
    config_get_global(&g);
    /* the scan interrupt is allocated on this core */
    scanner_start(s_q, config_active_scan_khz(), g.debounce_ms, g.enc_steps, g.enc_reverse);

    scan_event_t se;
    for (;;) {
        /* while the encoder button is held, wake up when it becomes a long press */
        TickType_t wait = portMAX_DELAY;
        if (s_btn_down && !s_btn_long) {
            TickType_t held = xTaskGetTickCount() - s_btn_since;
            TickType_t lp = pdMS_TO_TICKS(LONG_PRESS_MS);
            wait = held >= lp ? 0 : lp - held;
        }
        if (xQueueReceive(s_q, &se, wait) == pdTRUE) {
            /* presses are timed until their first report reaches the host */
            if (se.ev & SCAN_EV_PRESS) lat_mark(se.t_us);
            handle(se.ev);
            lat_clear(); /* a press that sent nothing must not time a later report */
        } else if (s_btn_down && !s_btn_long) {
            on_long_press();
        }
    }
}

void keyproc_start(void)
{
    s_q = xQueueCreate(64, sizeof(scan_event_t));
    xTaskCreatePinnedToCore(keyproc_task, "keyproc", 4096, NULL, KEYPROC_PRIO, NULL, KEYPROC_CORE);
    ESP_LOGI(TAG, "key processor started");
}

bool keyproc_selecting(void) { return s_selecting; }
int keyproc_pending_profile(void) { return s_pending; }

uint32_t keyproc_pressed_mask(void) { return scanner_state(); }
