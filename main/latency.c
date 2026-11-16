#include "latency.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_mark; /* 0 = none */

typedef struct {
    uint32_t v[LAT_SAMPLES];
    uint16_t n, head;
} ring_t;
static ring_t s_ring[2]; /* 0 = USB, 1 = BLE */

void lat_mark(uint32_t t_event_us) { s_mark = t_event_us ? t_event_us : 1; }
void lat_clear(void) { s_mark = 0; }

uint32_t lat_take_mark(void)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t m = s_mark;
    s_mark = 0;
    portEXIT_CRITICAL(&s_mux);
    return m;
}

void lat_add(bool ble, uint32_t dt_us)
{
    if (dt_us > 1000000) return; /* > 1 s: stalled transfer or host asleep, not a latency sample */
    ring_t *r = &s_ring[ble];
    portENTER_CRITICAL(&s_mux);
    r->v[r->head] = dt_us;
    r->head = (r->head + 1) % LAT_SAMPLES;
    if (r->n < LAT_SAMPLES) r->n++;
    portEXIT_CRITICAL(&s_mux);
}

void lat_estimate(bool ble, int scan_khz, uint32_t ble_itvl_us, lat_estimate_t *out)
{
    memset(out, 0, sizeof(*out));
    ring_t r;
    portENTER_CRITICAL(&s_mux);
    r = s_ring[ble];
    portEXIT_CRITICAL(&s_mux);

    if (r.n) {
        uint64_t sum = 0;
        for (int i = 0; i < r.n; i++) {
            sum += r.v[i];
            if (r.v[i] > out->dev_max_us) out->dev_max_us = r.v[i];
        }
        out->dev_avg_us = sum / r.n;
        out->measured = true;
        out->n = r.n;
    } else if (!ble) {
        out->dev_avg_us = 500;  /* model until the first press: half / one 1 ms USB poll */
        out->dev_max_us = 1000;
    }

    /* contact closes somewhere between two scan samples */
    const uint32_t scan_us = 1000u / (uint32_t)(scan_khz > 0 ? scan_khz : 1); /* 1 kHz -> 1000 us */
    out->avg_us = out->dev_avg_us + scan_us / 2;
    out->max_us = out->dev_max_us + scan_us;
    if (ble) { /* the notification waits for the next connection event */
        out->avg_us += ble_itvl_us / 2;
        out->max_us += ble_itvl_us;
    }
}

uint32_t lat_now_us(void) { return (uint32_t)esp_timer_get_time(); }
