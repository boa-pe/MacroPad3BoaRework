#pragma once
#include <stdint.h>
#include <stdbool.h>

/* NimBLE HID-over-GATT keyboard + consumer control. Starts the host stack but does not advertise
 * until ble_hid_set_enabled(true). */
void ble_hid_init(const char *name);
/* false: stop advertising and drop the connection (radio idle); true: advertise / accept hosts. */
void ble_hid_set_enabled(bool en);
void ble_hid_set_name(const char *name);
/* Connected, encrypted and the host subscribed to input reports. */
bool ble_hid_connected(void);
/* Current connection interval in microseconds (0 when not connected). */
uint32_t ble_hid_conn_interval_us(void);
bool ble_hid_send_keyboard(const uint8_t report[8]);
bool ble_hid_send_consumer(uint16_t usage);
bool ble_hid_send_system(uint8_t value); /* 0..3, see RID_SYSTEM */
/* Delete all stored pairings and drop the current connection. */
void ble_hid_forget_bonds(void);
