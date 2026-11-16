#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Turns scanner events into HID reports.
 *
 *  - ACT_KEY / ACT_MEDIA follow the physical key: pressed on press, released on release (so the
 *    host's own auto-repeat works and chords like Ctrl+C behave like a real keyboard).
 *  - Encoder left/right are taps: press + release report per detent.
 *  - ACT_TEXT types its string once on press.
 *  - Encoder button short press sends its own mapped action (on release).
 *  - Encoder button long press (1.5 s) enters profile selection mode:
 *      turn the encoder  -> show next / previous profile (LED blinks its colour)
 *      key 1..9          -> activate that profile and leave the mode
 *      short press       -> activate the shown profile and leave the mode
 *      long press        -> cancel, the previous profile stays active
 */
void keyproc_start(void);
/* Profile selection mode active, and the profile currently shown in it. */
bool keyproc_selecting(void);
int keyproc_pending_profile(void);
/* Bit n set while key n is down (debounced), including encoder switch. */
uint32_t keyproc_pressed_mask(void);
