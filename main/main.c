/*
 * MacroPad 3 - ESP-IDF firmware for a 9-key + rotary-encoder macropad on the Waveshare ESP32-S3-Zero.
 *
 *  scanner   GPTimer ISR (1/2/4/8/16 kHz) samples the keys, PCNT decodes the encoder
 *  keyproc   high-priority task on core 1: events -> HID reports, Fn/profile switching
 *  hid_out   USB HID (TinyUSB) when a host enumerated us, BLE HID (NimBLE) otherwise
 *  config    9 profiles (colour, scan rate, 12 key actions) + global settings in NVS
 *  web       Wi-Fi station + mDNS (http://macropad.local) or setup access point with captive portal
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "config.h"
#include "scanner.h"
#include "keyproc.h"
#include "hid_out.h"
#include "ble_hid.h"
#include "status_led.h"
#include "wifi_mgr.h"
#include "dns_server.h"
#include "web_server.h"
#include "auth.h"

static const char *TAG = "main";

static void on_config_changed(unsigned what)
{
    if (what & CFG_CHANGED_ACTIVE) scanner_set_rate(config_active_scan_khz());
    if (what & CFG_CHANGED_WIFI) {
        wifi_list_t l;
        config_get_wifi(&l);
        wifi_mgr_set_networks(&l);
    }
    if (what & CFG_CHANGED_GLOBAL) {
        global_cfg_t g;
        config_get_global(&g);
        scanner_set_debounce(g.debounce_ms);
        scanner_set_encoder(g.enc_steps, g.enc_reverse);
        ble_hid_set_name(g.ble_name);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* a freshly flashed OTA image is good once it got this far */
    esp_ota_mark_app_valid_cancel_rollback();

    config_init();
    auth_init();
    global_cfg_t g;
    config_get_global(&g);

    /* setup access point only when no network is saved, or key 1 is held at power-up */
    const bool key1_held = scanner_raw_key1_pressed();
    wifi_list_t nets;
    config_get_wifi(&nets);
    const bool setup = key1_held || nets.count == 0;
    if (key1_held) ESP_LOGI(TAG, "key 1 held at boot: Wi-Fi setup mode");

    status_led_init(setup);
    keyproc_start();
    hid_out_init();
    config_set_change_cb(on_config_changed);

    if (setup) {
        wifi_mgr_start_ap();
        dns_server_start();
        web_server_start(true);
    } else if (g.wifi_enabled) {
        wifi_mgr_start_sta(&nets, g.hostname);
        web_server_start(false);
    } else {
        ESP_LOGI(TAG, "Wi-Fi disabled (hold key 1 while plugging in to configure)");
    }
}
