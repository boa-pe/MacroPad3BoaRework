#include "auth.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG = "auth";

#define MAX_SESSIONS        4
#define SESSION_IDLE_US     (10LL * 60 * 1000000)  /* 10 min without user activity in the web UI */
#define MIN_START_GAP_US    (3LL * 1000000)        /* min age of a pending challenge before another may replace it */

typedef struct {
    char id[AUTH_ID_LEN + 1];
    uint8_t keys[AUTH_SEQ_LEN];
    int progress;
    auth_state_t state;
    int64_t expires;
} challenge_t;

typedef struct {
    char token[AUTH_TOKEN_LEN + 1];
    int64_t last_use;
} session_t;

static SemaphoreHandle_t s_lock;
static challenge_t s_ch;
static session_t s_sess[MAX_SESSIONS];
static int64_t s_last_start = -MIN_START_GAP_US;

void auth_init(void) { s_lock = xSemaphoreCreateMutex(); }

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static int64_t now_us(void) { return esp_timer_get_time(); }

/* hardware RNG (true random while Wi-Fi/BT is running) */
static void random_hex(char *out, int nchars)
{
    for (int i = 0; i < nchars; i += 8) {
        char buf[9];
        snprintf(buf, sizeof(buf), "%08lx", (unsigned long)esp_random());
        memcpy(out + i, buf, (nchars - i) < 8 ? (nchars - i) : 8);
    }
    out[nchars] = 0;
}

/* comparison time doesn't depend on where the strings differ */
static bool secure_eq(const char *a, const char *b, size_t n)
{
    if (strlen(a) != n || strlen(b) != n) return false;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

static void expire_locked(void)
{
    if (s_ch.state == AUTH_ST_PENDING && now_us() > s_ch.expires) {
        s_ch.state = AUTH_ST_EXPIRED;
        ESP_LOGI(TAG, "challenge expired");
    }
}

esp_err_t auth_start(char id[AUTH_ID_LEN + 1], uint8_t keys[AUTH_SEQ_LEN], int *ttl_s)
{
    lock();
    int64_t t = now_us();
    expire_locked();
    if (s_ch.state == AUTH_ST_PENDING && t - s_last_start < MIN_START_GAP_US) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_last_start = t;

    /* 4 different keys: partial Fisher-Yates shuffle of 0..8 */
    uint8_t pool[NUM_BUTTONS];
    for (int i = 0; i < NUM_BUTTONS; i++) pool[i] = i;
    for (int i = 0; i < AUTH_SEQ_LEN; i++) {
        int j = i + esp_random() % (NUM_BUTTONS - i);
        uint8_t tmp = pool[i];
        pool[i] = pool[j];
        pool[j] = tmp;
    }
    memcpy(s_ch.keys, pool, AUTH_SEQ_LEN);
    random_hex(s_ch.id, AUTH_ID_LEN);
    s_ch.progress = 0;
    s_ch.state = AUTH_ST_PENDING;
    s_ch.expires = t + AUTH_CHALLENGE_TTL_S * 1000000LL;

    memcpy(id, s_ch.id, AUTH_ID_LEN + 1);
    memcpy(keys, s_ch.keys, AUTH_SEQ_LEN);
    *ttl_s = AUTH_CHALLENGE_TTL_S;
    unlock();
    ESP_LOGI(TAG, "challenge started");
    return ESP_OK;
}

static void new_session_locked(char token[AUTH_TOKEN_LEN + 1])
{
    int slot = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) { /* free slot, else the least recently used */
        if (!s_sess[i].token[0]) { slot = i; break; }
        if (s_sess[i].last_use < s_sess[slot].last_use) slot = i;
    }
    random_hex(s_sess[slot].token, AUTH_TOKEN_LEN);
    s_sess[slot].last_use = now_us();
    memcpy(token, s_sess[slot].token, AUTH_TOKEN_LEN + 1);
}

auth_state_t auth_status(const char *id, int *progress, int *ttl_s, char token[AUTH_TOKEN_LEN + 1])
{
    token[0] = 0;
    lock();
    expire_locked();
    if (!id || !secure_eq(id, s_ch.id, AUTH_ID_LEN)) {
        unlock();
        return AUTH_ST_NONE;
    }
    auth_state_t st = s_ch.state;
    *progress = s_ch.progress;
    int64_t left = (s_ch.expires - now_us()) / 1000000;
    *ttl_s = left > 0 ? (int)left : 0;
    if (st == AUTH_ST_OK) {
        new_session_locked(token);
        memset(&s_ch, 0, sizeof(s_ch)); /* a challenge yields exactly one session */
        ESP_LOGI(TAG, "session issued");
    }
    unlock();
    return st;
}

void auth_cancel_id(const char *id)
{
    lock();
    if (id && secure_eq(id, s_ch.id, AUTH_ID_LEN) && s_ch.state == AUTH_ST_PENDING) s_ch.state = AUTH_ST_CANCELLED;
    unlock();
}

bool auth_capturing(void)
{
    lock();
    expire_locked();
    bool c = s_ch.state == AUTH_ST_PENDING;
    unlock();
    return c;
}

void auth_key_pressed(int key)
{
    lock();
    expire_locked();
    if (s_ch.state == AUTH_ST_PENDING) {
        if (key == s_ch.keys[s_ch.progress]) {
            if (++s_ch.progress == AUTH_SEQ_LEN) {
                s_ch.state = AUTH_ST_OK;
                ESP_LOGI(TAG, "challenge passed");
            }
        } else {
            s_ch.state = AUTH_ST_FAILED;
            ESP_LOGW(TAG, "challenge failed (wrong key)");
        }
    }
    unlock();
}

void auth_cancel(void)
{
    lock();
    if (s_ch.state == AUTH_ST_PENDING) {
        s_ch.state = AUTH_ST_CANCELLED;
        ESP_LOGI(TAG, "challenge cancelled on the device");
    }
    unlock();
}

bool auth_session_valid(const char *token, bool touch)
{
    if (!token || !token[0]) return false;
    bool ok = false;
    lock();
    int64_t t = now_us();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session_t *s = &s_sess[i];
        if (!s->token[0]) continue;
        if (t - s->last_use > SESSION_IDLE_US) { /* idle too long */
            memset(s, 0, sizeof(*s));
            continue;
        }
        if (secure_eq(token, s->token, AUTH_TOKEN_LEN)) {
            if (touch) s->last_use = t;
            ok = true;
        }
    }
    unlock();
    return ok;
}

void auth_logout(const char *token)
{
    lock();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sess[i].token[0] && secure_eq(token, s_sess[i].token, AUTH_TOKEN_LEN)) memset(&s_sess[i], 0, sizeof(s_sess[i]));
    }
    unlock();
}
