#include "status_led.h"
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "board.h"
#include "config.h"
#include "keyproc.h"
#include "hid_out.h"
#include "auth.h"

static led_strip_handle_t s_strip;
static bool s_setup;
static volatile bool s_ota;

static void hw_write(uint8_t r, uint8_t g, uint8_t b)
{
#if BOARD_LED_ORDER_RGB
    led_strip_set_pixel(s_strip, 0, g, r, b); /* driver sends G,R,B: swapped -> R,G,B on the wire */
#else
    led_strip_set_pixel(s_strip, 0, r, g, b);
#endif
    led_strip_refresh(s_strip);
}

static void led_task(void *arg)
{
    uint32_t last = 0xFFFFFFFF;
    for (;;) {
        int64_t ms = esp_timer_get_time() / 1000;
        uint8_t c[3];
        config_get_active_color(c);
        global_cfg_t g;
        config_get_global(&g);

        float k;
        if (s_ota) {
            c[0] = c[1] = c[2] = 255;
            k = (ms % 200) < 100 ? 1.f : 0.f;
        } else if (auth_capturing()) { /* web UI verification: white double flash */
            c[0] = c[1] = c[2] = 255;
            int t = ms % 1000;
            k = (t < 100 || (t >= 200 && t < 300)) ? 1.f : 0.f;
        } else if (keyproc_selecting()) { /* blink the colour of the profile being chosen */
            config_get_profile_color(keyproc_pending_profile(), c);
            k = (ms % 250) < 125 ? 1.f : 0.f; /* 4 Hz: clearly faster than the setup-mode blink */
        } else if (s_setup) {
            k = (ms % 1000) < 500 ? 1.f : 0.f;
        } else if (hid_out_link() != LINK_NONE) {
            k = 1.f;
        } else {
            k = 0.08f + 0.92f * (0.5f - 0.5f * cosf((ms % 2500) * 2.f * (float)M_PI / 2500.f));
        }
        k *= g.brightness / 255.f;
        uint8_t r = c[0] * k, gg = c[1] * k, b = c[2] * k;
        uint32_t out = (r << 16) | (gg << 8) | b;
        if (out != last) {
            hw_write(r, gg, b);
            last = out;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void status_led_init(bool setup_mode)
{
    s_setup = setup_mode;
    led_strip_config_t cfg = {
        .strip_gpio_num = BOARD_LED_PIN,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &s_strip));
    led_strip_clear(s_strip);
    xTaskCreatePinnedToCore(led_task, "led", 3072, NULL, 2, NULL, 0);
}

void status_led_set_ota(bool active) { s_ota = active; }
