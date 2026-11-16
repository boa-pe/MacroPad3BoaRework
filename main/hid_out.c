#include "hid_out.h"
#include "usb_hid.h"
#include "ble_hid.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "class/hid/hid_device.h"

static const char *TAG = "hid";

const uint8_t hid_report_map[] = { HID_REPORT_MAP_BYTES };
const size_t hid_report_map_len = sizeof(hid_report_map);

/* How long to wait for USB enumeration at boot before deciding to start BLE. Hosts normally
 * enumerate in 100-500 ms; a charger never does. */
#define USB_ENUM_WAIT_MS 1500

static TaskHandle_t s_link_task;
static volatile bool s_ble_enabled;

hid_link_t hid_out_link(void)
{
    if (usb_hid_mounted()) return LINK_USB;
    if (ble_hid_connected()) return LINK_BLE;
    return LINK_NONE;
}

bool hid_out_usb_mounted(void) { return usb_hid_mounted(); }
bool hid_out_ble_enabled(void) { return s_ble_enabled; }
bool hid_out_ble_connected(void) { return ble_hid_connected(); }

bool hid_out_keyboard(uint8_t mods, const uint8_t keys[6])
{
    uint8_t rep[8] = { mods, 0, keys[0], keys[1], keys[2], keys[3], keys[4], keys[5] };
    /* USB has priority; BLE is switched off while USB is attached anyway */
    if (usb_hid_mounted()) return usb_hid_send(RID_KEYBOARD, rep, sizeof(rep));
    if (ble_hid_connected()) return ble_hid_send_keyboard(rep);
    return false;
}

bool hid_out_consumer(uint16_t usage)
{
    if (usb_hid_mounted()) return usb_hid_send(RID_CONSUMER, &usage, sizeof(usage));
    if (ble_hid_connected()) return ble_hid_send_consumer(usage);
    return false;
}

bool hid_out_system(uint16_t usage)
{
    uint8_t v = (usage >= 0x81 && usage <= 0x83) ? usage - 0x80 : 0; /* array index into the 3 usages */
    if (usb_hid_mounted()) return usb_hid_send(RID_SYSTEM, &v, 1);
    if (ble_hid_connected()) return ble_hid_send_system(v);
    return false;
}

void hid_out_usb_state_changed(void)
{
    if (s_link_task) xTaskNotifyGive(s_link_task);
}

static void set_ble(bool en)
{
    if (en == s_ble_enabled) return;
    s_ble_enabled = en;
    ESP_LOGI(TAG, "%s", en ? "no USB host: Bluetooth on" : "USB host attached: Bluetooth off");
    ble_hid_set_enabled(en);
}

/* Follows the USB state and switches BLE accordingly (BLE calls stay out of the TinyUSB task). */
static void link_task(void *arg)
{
    TickType_t t0 = xTaskGetTickCount();
    while (!usb_hid_mounted() && xTaskGetTickCount() - t0 < pdMS_TO_TICKS(USB_ENUM_WAIT_MS)) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
    set_ble(!usb_hid_mounted()); /* BLE starts disabled, so this only acts without USB */

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* a quick unplug/replug or bus reset shouldn't bring BLE up: let it settle */
        vTaskDelay(pdMS_TO_TICKS(300));
        ulTaskNotifyTake(pdTRUE, 0);
        set_ble(!usb_hid_mounted());
    }
}

void hid_out_init(void)
{
    global_cfg_t g;
    config_get_global(&g);
    ble_hid_init(g.ble_name); /* host stack up, but not advertising until enabled */
    usb_hid_init();
    xTaskCreatePinnedToCore(link_task, "hid_link", 3072, NULL, 6, &s_link_task, 0);
}
