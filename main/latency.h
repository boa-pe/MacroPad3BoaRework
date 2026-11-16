#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * Runtime latency measurement.
 *
 * The scan ISR timestamps every key event. The key processor marks the event it is handling;
 * the first report sent for it takes the mark:
 *   USB: measured until the host has actually collected the report (TinyUSB transfer complete),
 *        so it includes the wait for the host's 1 ms poll.
 *   BLE: measured until the notification is queued in the Bluetooth stack; the connection
 *        interval is added as a model (the radio doesn't report per-notification delivery).
 * The time between the contact closing and the next scan sample (up to one scan period) is
 * added as a model too. What happens inside the host after the report arrives isn't visible.
 */
#define LAT_SAMPLES 32

/* key processor: event being handled (timestamp from the ISR); lat_clear() when done */
void lat_mark(uint32_t t_event_us);
void lat_clear(void);
/* transports: take the mark for the report being sent (0 = no measurement) */
uint32_t lat_take_mark(void);
void lat_add(bool ble, uint32_t dt_us);
uint32_t lat_now_us(void); /* same clock as the ISR timestamps (low 32 bits of esp_timer, us) */

typedef struct {
    bool measured;     /* false: no samples yet, values are a model only */
    uint16_t n;        /* samples in the window (last LAT_SAMPLES key presses) */
    uint32_t avg_us;   /* estimated average key-to-host latency */
    uint32_t max_us;   /* estimated worst case */
    uint32_t dev_avg_us, dev_max_us; /* measured part only */
} lat_estimate_t;

void lat_estimate(bool ble, int scan_khz, uint32_t ble_itvl_us, lat_estimate_t *out);
