#include "usb_hid.h"
#include "hid_out.h"
#include "latency.h"
#include "board.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"

static const char *TAG = "usb";

#define EP_HID_IN        0x81
#define EP_HID_SIZE      16
#define EP_HID_INTERVAL  1   /* ms: the fastest a full-speed device can be polled */
#define SEND_TIMEOUT_MS  50

#define CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CFG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, sizeof((const uint8_t[]){ HID_REPORT_MAP_BYTES }), EP_HID_IN, EP_HID_SIZE,
                       EP_HID_INTERVAL),
};

static const tusb_desc_device_t s_dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,  /* Espressif */
    .idProduct = 0x4004, /* TinyUSB HID-only default PID */
    .bcdDevice = 0x0300,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static char s_serial[13];
static const char *s_str_desc[] = {
    (const char[]){ 0x09, 0x04 }, /* English */
    "MacroPad",
    "MacroPad 3",
    s_serial,
    "MacroPad 3 Keyboard",
};

static SemaphoreHandle_t s_tx_done;
static volatile bool s_mounted;
static volatile uint8_t s_leds;
static volatile uint32_t s_inflight_t0; /* event time of the report in flight (0 = not timed) */

/* ---------- TinyUSB callbacks (run in the TinyUSB task) ---------- */

void tud_mount_cb(void)
{
    s_mounted = true;
    ESP_LOGI(TAG, "mounted by host");
    hid_out_usb_state_changed();
}

void tud_umount_cb(void)
{
    s_mounted = false;
    ESP_LOGI(TAG, "unmounted");
    hid_out_usb_state_changed();
}

void tud_suspend_cb(bool remote_wakeup_en) { ESP_LOGI(TAG, "suspended (remote wakeup %s)", remote_wakeup_en ? "on" : "off"); }
void tud_resume_cb(void) { ESP_LOGI(TAG, "resumed"); }

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return hid_report_map;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    if (report_type != HID_REPORT_TYPE_OUTPUT || bufsize < 1) return;
    /* with report IDs the ID may arrive either in report_id or as the first byte */
    if (report_id == RID_KEYBOARD) {
        s_leds = buffer[0];
    } else if (report_id == 0 && bufsize >= 2 && buffer[0] == RID_KEYBOARD) {
        s_leds = buffer[1];
    }
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    uint32_t t0 = s_inflight_t0;
    if (t0) {
        s_inflight_t0 = 0;
        lat_add(false, lat_now_us() - t0);
    }
    xSemaphoreGive(s_tx_done);
}

/* ---------- API ---------- */

bool usb_hid_mounted(void) { return s_mounted; }
bool usb_hid_suspended(void) { return tud_suspended(); }
uint8_t usb_hid_leds(void) { return s_leds; }

bool usb_hid_send(uint8_t report_id, const void *data, uint16_t len)
{
    if (!s_mounted) return false;
    if (tud_suspended()) {
        /* host is asleep: a key press wakes it (if it allowed remote wakeup); the key itself is dropped */
        tud_remote_wakeup();
        return false;
    }
    TickType_t t0 = xTaskGetTickCount();
    while (!tud_hid_ready()) {
        if (!s_mounted || xTaskGetTickCount() - t0 > pdMS_TO_TICKS(SEND_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "endpoint busy, report dropped");
            return false;
        }
        xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(2));
    }
    xSemaphoreTake(s_tx_done, 0); /* clear a stale completion */
    s_inflight_t0 = lat_take_mark(); /* set before queuing: the completion can't run earlier (lower prio, same core) */
    if (tud_hid_report(report_id, data, len)) return true;
    s_inflight_t0 = 0;
    return false;
}

void usb_hid_init(void)
{
    s_tx_done = xSemaphoreCreateBinary();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const tinyusb_config_t cfg = {
        .device_descriptor = &s_dev_desc,
        .string_descriptor = s_str_desc,
        .string_descriptor_count = sizeof(s_str_desc) / sizeof(s_str_desc[0]),
        .external_phy = false,
        .configuration_descriptor = s_cfg_desc,
#if BOARD_VBUS_SENSE_PIN >= 0
        .self_powered = true,
        .vbus_monitor_io = BOARD_VBUS_SENSE_PIN,
#endif
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    ESP_LOGI(TAG, "TinyUSB HID started (poll interval %d ms)", EP_HID_INTERVAL);
}
