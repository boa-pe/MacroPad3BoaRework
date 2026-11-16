#pragma once
#include <stdbool.h>

/*
 * The LED always shows the active profile's colour; the pattern tells the state:
 *   solid        - a host is connected (USB or BLE)
 *   breathing    - no host connected (BLE advertising / waiting for USB)
 *   fast blink   - (4 Hz) profile selection mode (long press on the encoder button); the colour is the
 *                  profile currently shown, which becomes active on a short press or key 1..9
 *   slow blink   - Wi-Fi setup access point is running
 *   white double flash - web UI verification: press the 4 keys shown in the browser
 *   white flash  - OTA update in progress
 */
void status_led_init(bool setup_mode);
void status_led_set_ota(bool active);
