#pragma once
#include <stdint.h>
#include <stdbool.h>

void usb_hid_init(void);
/* Enumerated by a host (stays true while the host is suspended). */
bool usb_hid_mounted(void);
bool usb_hid_suspended(void);
/* Queue one input report; waits (max ~50 ms) for the previous one to be collected by the host. */
bool usb_hid_send(uint8_t report_id, const void *data, uint16_t len);
/* Keyboard LED state from the host (bit0 NumLock, bit1 CapsLock, bit2 ScrollLock). */
uint8_t usb_hid_leds(void);
