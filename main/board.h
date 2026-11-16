#pragma once
/*
 * Waveshare ESP32-S3-Zero pin map (taken from the original Arduino macropad sketch).
 * All inputs are active-low with internal pull-ups.
 */

/* 9 key switches, index 0..8 = key 1..9 */
#define BOARD_KEY_PIN_LIST  1, 2, 3, 4, 5, 6, 7, 8, 17

/* Rotary encoder (quadrature A/B) and its push switch */
#define BOARD_ENC_A_PIN     9
#define BOARD_ENC_B_PIN     10
#define BOARD_ENC_SW_PIN    38

/* On-board WS2812 RGB LED. The S3-Zero part expects R,G,B byte order (NEO_RGB in the sketch). */
#define BOARD_LED_PIN       21
#define BOARD_LED_ORDER_RGB 1

/* Holding key 1 while powering up starts the Wi-Fi setup access point (as in the sketch). */
#define BOARD_SETUP_KEY_PIN 1

/* Optional USB VBUS sense input (through a divider). -1 = not wired: "USB connected" then means
 * "enumerated by a host"; a plain charger never enumerates, so BLE stays active on it. */
#define BOARD_VBUS_SENSE_PIN (-1)
