#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define NUM_PROFILES   9
#define NUM_BUTTONS    9
#define KEY_ENC_CCW    9   /* virtual key: encoder rotated left  */
#define KEY_ENC_CW     10  /* virtual key: encoder rotated right */
#define KEY_ENC_SW     11  /* encoder push switch (also Fn for profile selection) */
#define NUM_KEYS       12
#define TEXT_MAX       48
#define SCAN_KHZ_MIN   1
#define SCAN_KHZ_MAX   16  /* steps double: 1, 2, 4, 8, 16 kHz */

typedef enum {
    ACT_NONE  = 0,
    ACT_KEY   = 1, /* modifiers + one keyboard usage, held while the key is held */
    ACT_MEDIA = 2, /* consumer-control usage (volume, play/pause, ...) */
    ACT_TEXT  = 3, /* types an ASCII string (US layout) on press */
    ACT_SYSTEM = 4, /* system control: usage 0x81 power off, 0x82 sleep, 0x83 wake up */
} action_type_t;

typedef struct {
    uint8_t type;        /* action_type_t */
    uint8_t mods;        /* HID modifier bits (ACT_KEY) */
    uint8_t key;         /* HID keyboard usage (ACT_KEY); 0 = modifiers only */
    uint8_t _pad;
    uint16_t usage;      /* consumer usage (ACT_MEDIA) or system usage 0x81..0x83 (ACT_SYSTEM) */
    char text[TEXT_MAX]; /* ACT_TEXT */
} key_action_t;

typedef struct {
    char name[20];
    uint8_t color[3];
    uint8_t scan_khz;    /* 1, 2, 4, 8 or 16 */
    key_action_t keys[NUM_KEYS];
} profile_t;

typedef struct {
    uint8_t active;      /* active profile 0..8 */
    uint8_t debounce_ms; /* 1..30 */
    uint8_t brightness;  /* LED brightness 1..255 */
    uint8_t enc_steps;   /* quadrature counts per detent: 1, 2 or 4 */
    bool enc_reverse;
    bool wifi_enabled;
    char ble_name[30];
    char hostname[32];
    char wifi_ssid[33]; /* legacy single network: migrated into the network list, then unused */
    char wifi_pass[65];
} global_cfg_t;

#define WIFI_MAX_NETS 5
typedef struct {
    char ssid[33];
    char pass[65];
} wifi_net_t;

typedef struct {
    uint8_t count;
    wifi_net_t net[WIFI_MAX_NETS];
} wifi_list_t;

/* Load from NVS (or defaults). Call once after nvs_flash_init(). */
void config_init(void);

/* Called (in the caller's context) after anything changed. what: CFG_CHANGED_* bits */
#define CFG_CHANGED_ACTIVE  1
#define CFG_CHANGED_PROFILE 2
#define CFG_CHANGED_GLOBAL  4
#define CFG_CHANGED_WIFI    8
typedef void (*config_change_cb_t)(unsigned what);
void config_set_change_cb(config_change_cb_t cb);

/* Snapshots (copied under a lock; safe from any task) */
void config_get_global(global_cfg_t *out);
void config_get_profile(int idx, profile_t *out);
int config_active(void);
void config_get_action(int key, key_action_t *out); /* from the active profile */
void config_get_active_color(uint8_t rgb[3]);
void config_get_profile_color(int idx, uint8_t rgb[3]);
int config_active_scan_khz(void);

/* Updates: applied immediately, written to flash in the background */
esp_err_t config_set_active(int idx);
esp_err_t config_set_global(const global_cfg_t *g);
esp_err_t config_set_profile(int idx, const profile_t *p);
/* Saved Wi-Fi networks (up to WIFI_MAX_NETS, empty SSIDs are dropped) */
void config_get_wifi(wifi_list_t *out);
esp_err_t config_set_wifi(const wifi_list_t *l);
/* Write pending changes to flash now (before a reboot). */
void config_flush(void);
void config_factory_reset(void);

/* Defaults (used for reset and by the web UI "reset profile") */
void config_default_profile(int idx, profile_t *out);
