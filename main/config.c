#include "config.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "esp_log.h"
#include "class/hid/hid.h"

static const char *TAG = "config";

#define NVS_NS      "macropad"
#define CFG_VERSION 1

#define SAVE_GLOBAL      (1u << 0)
#define SAVE_PROFILE(i)  (1u << (1 + (i)))
#define SAVE_WIFI        (1u << 10)
#define SAVE_DELAY_MS    1500

static global_cfg_t s_global;
static profile_t s_prof[NUM_PROFILES];
static wifi_list_t s_wifi;
static SemaphoreHandle_t s_lock;
static config_change_cb_t s_cb;
static TaskHandle_t s_save_task;
static volatile uint32_t s_dirty;

static const uint8_t k_colors[NUM_PROFILES][3] = {
    { 0, 255, 0 }, { 255, 0, 0 }, { 0, 0, 255 }, { 255, 255, 0 }, { 255, 0, 255 },
    { 0, 255, 255 }, { 255, 128, 0 }, { 128, 0, 255 }, { 255, 255, 255 },
};

static void set_key(key_action_t *a, uint8_t mods, uint8_t key)
{
    memset(a, 0, sizeof(*a));
    a->type = ACT_KEY;
    a->mods = mods;
    a->key = key;
}

static void set_media(key_action_t *a, uint16_t usage)
{
    memset(a, 0, sizeof(*a));
    a->type = ACT_MEDIA;
    a->usage = usage;
}

void config_default_profile(int idx, profile_t *p)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "Profile %d", idx + 1);
    memcpy(p->color, k_colors[idx], 3);
    p->scan_khz = 4;
    /* profiles 2..9 start empty: every key (and the encoder) is ACT_NONE from the memset */
    if (idx == 0) {
        strcpy(p->name, "Numpad");
        static const uint8_t num[9] = { HID_KEY_KEYPAD_1, HID_KEY_KEYPAD_2, HID_KEY_KEYPAD_3,
                                        HID_KEY_KEYPAD_4, HID_KEY_KEYPAD_5, HID_KEY_KEYPAD_6,
                                        HID_KEY_KEYPAD_7, HID_KEY_KEYPAD_8, HID_KEY_KEYPAD_9 };
        for (int i = 0; i < 9; i++) set_key(&p->keys[i], 0, num[i]);
        set_media(&p->keys[KEY_ENC_CCW], HID_USAGE_CONSUMER_VOLUME_DECREMENT);
        set_media(&p->keys[KEY_ENC_CW], HID_USAGE_CONSUMER_VOLUME_INCREMENT);
        set_media(&p->keys[KEY_ENC_SW], HID_USAGE_CONSUMER_MUTE);
    }
}

static void default_global(global_cfg_t *g)
{
    memset(g, 0, sizeof(*g));
    g->active = 0;
    g->debounce_ms = 5;
    g->brightness = 60;
    g->enc_steps = 4;
    g->enc_reverse = false;
    g->wifi_enabled = true;
    strcpy(g->ble_name, "MacroPad 3");
    strcpy(g->hostname, "macropad");
}

/* Clamp/repair values so the rest of the firmware can trust them. */
static void sanitize_global(global_cfg_t *g)
{
    if (g->active >= NUM_PROFILES) g->active = 0;
    if (g->debounce_ms < 1) g->debounce_ms = 1;
    if (g->debounce_ms > 30) g->debounce_ms = 30;
    if (g->brightness < 1) g->brightness = 1;
    if (g->enc_steps != 1 && g->enc_steps != 2 && g->enc_steps != 4) g->enc_steps = 4;
    g->ble_name[sizeof(g->ble_name) - 1] = 0;
    g->hostname[sizeof(g->hostname) - 1] = 0;
    g->wifi_ssid[sizeof(g->wifi_ssid) - 1] = 0;
    g->wifi_pass[sizeof(g->wifi_pass) - 1] = 0;
    if (!g->ble_name[0]) strcpy(g->ble_name, "MacroPad 3");
    if (!g->hostname[0]) strcpy(g->hostname, "macropad");
}

static void sanitize_profile(profile_t *p)
{
    p->name[sizeof(p->name) - 1] = 0;
    /* only powers of two are valid: round down (older configs may hold 3, 5, 10, ...) */
    if (p->scan_khz < SCAN_KHZ_MIN) p->scan_khz = SCAN_KHZ_MIN;
    if (p->scan_khz > SCAN_KHZ_MAX) p->scan_khz = SCAN_KHZ_MAX;
    while (p->scan_khz & (p->scan_khz - 1)) p->scan_khz &= p->scan_khz - 1;
    for (int k = 0; k < NUM_KEYS; k++) {
        key_action_t *a = &p->keys[k];
        if (a->type > ACT_SYSTEM) a->type = ACT_NONE;
        if (a->type == ACT_SYSTEM && (a->usage < 0x81 || a->usage > 0x83)) a->type = ACT_NONE;
        a->text[TEXT_MAX - 1] = 0;
        a->_pad = 0;
    }
}

/* Terminate strings and compact the list (no empty SSIDs, no duplicates). */
static void sanitize_wifi(wifi_list_t *l)
{
    wifi_list_t out = { 0 };
    int n = l->count > WIFI_MAX_NETS ? WIFI_MAX_NETS : l->count;
    for (int i = 0; i < n; i++) {
        wifi_net_t w = l->net[i];
        w.ssid[sizeof(w.ssid) - 1] = 0;
        w.pass[sizeof(w.pass) - 1] = 0;
        if (!w.ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < out.count && !dup; j++) dup = strcmp(out.net[j].ssid, w.ssid) == 0;
        if (!dup) out.net[out.count++] = w;
    }
    *l = out;
}

/* ---------- persistence ---------- */

static void save_now(uint32_t what)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "ver", CFG_VERSION);
    if (what & SAVE_GLOBAL) {
        global_cfg_t g;
        config_get_global(&g);
        nvs_set_blob(h, "global", &g, sizeof(g));
    }
    if (what & SAVE_WIFI) {
        wifi_list_t w;
        config_get_wifi(&w);
        nvs_set_blob(h, "wifi", &w, sizeof(w));
    }
    for (int i = 0; i < NUM_PROFILES; i++) {
        if (!(what & SAVE_PROFILE(i))) continue;
        profile_t p;
        config_get_profile(i, &p);
        char key[8];
        snprintf(key, sizeof(key), "p%d", i);
        nvs_set_blob(h, key, &p, sizeof(p));
    }
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved (0x%03" PRIx32 "): %s", what, esp_err_to_name(err));
}

/* Coalesces bursts of changes (e.g. spinning through profiles) into one flash write. */
static void save_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SAVE_DELAY_MS))) { }
        uint32_t what = __atomic_exchange_n(&s_dirty, 0, __ATOMIC_SEQ_CST);
        if (what) save_now(what);
    }
}

static void mark_dirty(uint32_t what)
{
    __atomic_fetch_or(&s_dirty, what, __ATOMIC_SEQ_CST);
    if (s_save_task) xTaskNotifyGive(s_save_task);
}

void config_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    default_global(&s_global);
    for (int i = 0; i < NUM_PROFILES; i++) config_default_profile(i, &s_prof[i]);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t ver = 0;
        nvs_get_u8(h, "ver", &ver);
        if (ver == CFG_VERSION) {
            size_t len = sizeof(global_cfg_t);
            global_cfg_t g;
            if (nvs_get_blob(h, "global", &g, &len) == ESP_OK && len == sizeof(g)) s_global = g;
            for (int i = 0; i < NUM_PROFILES; i++) {
                char key[8];
                snprintf(key, sizeof(key), "p%d", i);
                profile_t p;
                len = sizeof(p);
                if (nvs_get_blob(h, key, &p, &len) == ESP_OK && len == sizeof(p)) s_prof[i] = p;
            }
            wifi_list_t w;
            len = sizeof(w);
            if (nvs_get_blob(h, "wifi", &w, &len) == ESP_OK && len == sizeof(w)) s_wifi = w;
        } else {
            ESP_LOGW(TAG, "no stored config (version %u), using defaults", ver);
        }
        nvs_close(h);
    }
    sanitize_global(&s_global);
    for (int i = 0; i < NUM_PROFILES; i++) sanitize_profile(&s_prof[i]);
    sanitize_wifi(&s_wifi);
    bool migrate = s_wifi.count == 0 && s_global.wifi_ssid[0];
    if (migrate) { /* firmware with a single network: move it into the list */
        strlcpy(s_wifi.net[0].ssid, s_global.wifi_ssid, sizeof(s_wifi.net[0].ssid));
        strlcpy(s_wifi.net[0].pass, s_global.wifi_pass, sizeof(s_wifi.net[0].pass));
        s_wifi.count = 1;
    }
    s_global.wifi_ssid[0] = s_global.wifi_pass[0] = 0;
    xTaskCreate(save_task, "cfg_save", 3072, NULL, 2, &s_save_task);
    if (migrate) mark_dirty(SAVE_WIFI | SAVE_GLOBAL);
    ESP_LOGI(TAG, "active profile %d (%s)", s_global.active + 1, s_prof[s_global.active].name);
}

void config_set_change_cb(config_change_cb_t cb) { s_cb = cb; }

static void notify(unsigned what)
{
    if (s_cb) s_cb(what);
}

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

void config_get_global(global_cfg_t *out)
{
    LOCK();
    *out = s_global;
    UNLOCK();
}

void config_get_profile(int idx, profile_t *out)
{
    if (idx < 0 || idx >= NUM_PROFILES) idx = 0;
    LOCK();
    *out = s_prof[idx];
    UNLOCK();
}

int config_active(void) { return s_global.active; }

void config_get_action(int key, key_action_t *out)
{
    LOCK();
    *out = s_prof[s_global.active].keys[key];
    UNLOCK();
}

void config_get_active_color(uint8_t rgb[3])
{
    LOCK();
    memcpy(rgb, s_prof[s_global.active].color, 3);
    UNLOCK();
}

void config_get_profile_color(int idx, uint8_t rgb[3])
{
    if (idx < 0 || idx >= NUM_PROFILES) idx = 0;
    LOCK();
    memcpy(rgb, s_prof[idx].color, 3);
    UNLOCK();
}

int config_active_scan_khz(void) { return s_prof[s_global.active].scan_khz; }

esp_err_t config_set_active(int idx)
{
    if (idx < 0 || idx >= NUM_PROFILES) return ESP_ERR_INVALID_ARG;
    if (idx == s_global.active) return ESP_OK;
    LOCK();
    s_global.active = idx;
    UNLOCK();
    ESP_LOGI(TAG, "profile %d (%s)", idx + 1, s_prof[idx].name);
    mark_dirty(SAVE_GLOBAL);
    notify(CFG_CHANGED_ACTIVE);
    return ESP_OK;
}

esp_err_t config_set_global(const global_cfg_t *g)
{
    global_cfg_t n = *g;
    sanitize_global(&n);
    LOCK();
    bool active_changed = n.active != s_global.active;
    s_global = n;
    UNLOCK();
    mark_dirty(SAVE_GLOBAL);
    notify(CFG_CHANGED_GLOBAL | (active_changed ? CFG_CHANGED_ACTIVE : 0));
    return ESP_OK;
}

esp_err_t config_set_profile(int idx, const profile_t *p)
{
    if (idx < 0 || idx >= NUM_PROFILES) return ESP_ERR_INVALID_ARG;
    profile_t n = *p;
    sanitize_profile(&n);
    LOCK();
    s_prof[idx] = n;
    UNLOCK();
    mark_dirty(SAVE_PROFILE(idx));
    notify(idx == s_global.active ? (CFG_CHANGED_PROFILE | CFG_CHANGED_ACTIVE) : CFG_CHANGED_PROFILE);
    return ESP_OK;
}

void config_get_wifi(wifi_list_t *out)
{
    LOCK();
    *out = s_wifi;
    UNLOCK();
}

esp_err_t config_set_wifi(const wifi_list_t *l)
{
    wifi_list_t n = *l;
    sanitize_wifi(&n);
    LOCK();
    s_wifi = n;
    UNLOCK();
    mark_dirty(SAVE_WIFI);
    notify(CFG_CHANGED_WIFI);
    return ESP_OK;
}

void config_flush(void)
{
    uint32_t what = __atomic_exchange_n(&s_dirty, 0, __ATOMIC_SEQ_CST);
    if (what) save_now(what);
}

void config_factory_reset(void)
{
    __atomic_store_n(&s_dirty, 0, __ATOMIC_SEQ_CST); /* drop pending saves */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}
