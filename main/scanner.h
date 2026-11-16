#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*
 * Hardware key scanning.
 *
 * - Keys 1..9 and the encoder switch are sampled by a GPTimer interrupt at 1, 2, 4, 8 or 16 kHz. The ISR reads
 *   the GPIO input registers directly and debounces with an "eager" algorithm: a change is reported
 *   on the very first sample that differs, then the key is locked for the debounce time. Latency from
 *   contact to event is therefore one scan period at most (1 ms at 1 kHz, 62.5 us at 16 kHz).
 * - The encoder is decoded by the PCNT hardware quadrature counter (with glitch filter). Its
 *   watch-point interrupt emits one event per detent as the virtual keys KEY_ENC_CCW / KEY_ENC_CW.
 *
 * Events are pushed from the ISRs to a FreeRTOS queue as scan_event_t: key index | SCAN_EV_PRESS,
 * plus the time it was detected.
 * Encoder events only ever come as presses (the consumer turns them into taps).
 */
#define SCAN_EV_PRESS 0x80
#define SCAN_EV_KEY(e) ((e) & 0x7F)

/* queue item: event byte + detection time (low 32 bits of esp_timer, us) for latency measurement */
typedef struct {
    uint8_t ev;
    uint32_t t_us;
} scan_event_t;

/* Must be called from the task/core that should service the scan interrupt. */
void scanner_start(QueueHandle_t q, int khz, int debounce_ms, int enc_steps, bool enc_reverse);

void scanner_set_rate(int khz);
void scanner_set_debounce(int debounce_ms);
void scanner_set_encoder(int steps, bool reverse);

/* Debounced state of the physical keys: bit n = key n held (n = 0..8, KEY_ENC_SW). */
uint32_t scanner_state(void);
int scanner_rate(void);
/* Number of encoder detents seen per direction since boot (for the web UI). */
void scanner_enc_counts(uint32_t *ccw, uint32_t *cw);
/* Read a raw pin level once (used at boot, before the scanner runs). */
bool scanner_raw_key1_pressed(void);
