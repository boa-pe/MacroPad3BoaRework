#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* One report map shared by USB and BLE: keyboard (ID 1, 6KRO + LED output), consumer (ID 2),
 * system control (ID 3: power down / sleep / wake up). */
#define RID_KEYBOARD 1
#define RID_CONSUMER 2
#define RID_SYSTEM   3  /* system control: 1 = power down, 2 = sleep, 3 = wake up, 0 = none */
/* expands to the descriptor bytes (needs class/hid/hid_device.h); used where the size must be constant */
#define HID_REPORT_MAP_BYTES     TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(RID_KEYBOARD)),     TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(RID_CONSUMER)), TUD_HID_REPORT_DESC_SYSTEM_CONTROL(HID_REPORT_ID(RID_SYSTEM))
extern const uint8_t hid_report_map[];
extern const size_t hid_report_map_len;

typedef enum { LINK_NONE, LINK_USB, LINK_BLE } hid_link_t;

/* Starts USB, waits briefly for enumeration and starts BLE only if no USB host showed up.
 * Afterwards BLE follows USB: off while a USB host is attached, on otherwise. */
void hid_out_init(void);

/* Send one full report to the active link. Blocks until the transport accepted it, so a
 * sequence of reports (press/release) is never coalesced. Returns false if nothing is connected. */
bool hid_out_keyboard(uint8_t mods, const uint8_t keys[6]);
bool hid_out_consumer(uint16_t usage);
/* usage: HID_USAGE_DESKTOP_SYSTEM_POWER_DOWN / _SLEEP / _WAKE_UP (0x81..0x83), 0 = released */
bool hid_out_system(uint16_t usage);

hid_link_t hid_out_link(void);
bool hid_out_usb_mounted(void);
bool hid_out_ble_enabled(void);
bool hid_out_ble_connected(void);

/* Called by the transports (any task) */
void hid_out_usb_state_changed(void);
