#include "wifi_mgr.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mdns.h"

static const char *TAG = "wifi";

#define BIT_GOT_IP  BIT0
#define BIT_DISC    BIT1
#define BIT_CHANGED BIT2  /* saved network list changed */
#define MAX_SCAN    20
#define CONNECT_TIMEOUT_MS 15000

static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_scan_lock, s_nets_lock;
static volatile bool s_connected;
static bool s_ap_mode;
static char s_ip[16] = "0.0.0.0";
static char s_ap_ssid[33];
static char s_sta_ssid[33];
static wifi_list_t s_nets;
static wifi_ap_record_t s_scan[MAX_SCAN];
static uint16_t s_scan_n;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_ap_mode) {
            s_connected = false;
            strcpy(s_ip, "0.0.0.0");
            xEventGroupSetBits(s_ev, BIT_DISC);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "connected to '%s', IP %s", s_sta_ssid, s_ip);
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

static void common_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    s_ev = xEventGroupCreate();
    s_scan_lock = xSemaphoreCreateMutex();
    s_nets_lock = xSemaphoreCreateMutex();
}

/* Scan into s_scan (caller must not hold s_scan_lock). */
static void do_scan(void)
{
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    wifi_scan_config_t sc = { .show_hidden = false };
    uint16_t n = 0;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        n = MAX_SCAN;
        if (esp_wifi_scan_get_ap_records(&n, s_scan) != ESP_OK) n = 0;
    }
    s_scan_n = n;
    xSemaphoreGive(s_scan_lock);
}

/* Order saved networks: visible ones by signal strength, then the ones not seen (maybe hidden). */
static int candidate_order(const wifi_list_t *l, int *order)
{
    int rssi[WIFI_MAX_NETS];
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    for (int i = 0; i < l->count; i++) {
        rssi[i] = -1000;
        for (int k = 0; k < s_scan_n; k++) {
            if (strcmp((const char *)s_scan[k].ssid, l->net[i].ssid) == 0 && s_scan[k].rssi > rssi[i]) {
                rssi[i] = s_scan[k].rssi;
            }
        }
    }
    xSemaphoreGive(s_scan_lock);
    for (int i = 0; i < l->count; i++) order[i] = i;
    for (int a = 1; a < l->count; a++) { /* stable insertion sort, strongest first */
        int v = order[a], b = a - 1;
        while (b >= 0 && rssi[order[b]] < rssi[v]) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = v;
    }
    return l->count;
}

static bool try_connect(const wifi_net_t *n)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, n->ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, n->pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = n->pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    strlcpy(s_sta_ssid, n->ssid, sizeof(s_sta_ssid));
    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_DISC);
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK || esp_wifi_connect() != ESP_OK) return false;
    ESP_LOGI(TAG, "trying '%s'", n->ssid);
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_DISC, pdFALSE, pdFALSE, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if (b & BIT_GOT_IP) return true;
    if (!(b & BIT_DISC)) { /* stuck (e.g. no DHCP lease): drop it before the next candidate */
        esp_wifi_disconnect();
        xEventGroupWaitBits(s_ev, BIT_DISC, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
    }
    return false;
}

/* Connection manager. Back-off 1, 2, 4 ... 60 s when nothing is reachable, so the shared 2.4 GHz
 * radio is left to Bluetooth most of the time. */
static void sta_task(void *arg)
{
    static wifi_list_t nets;
    int backoff = 0;
    for (;;) {
        xSemaphoreTake(s_nets_lock, portMAX_DELAY);
        nets = s_nets;
        xSemaphoreGive(s_nets_lock);
        xEventGroupClearBits(s_ev, BIT_CHANGED);

        if (nets.count == 0) {
            s_sta_ssid[0] = 0;
            ESP_LOGW(TAG, "no saved networks");
            xEventGroupWaitBits(s_ev, BIT_CHANGED, pdFALSE, pdFALSE, portMAX_DELAY);
            continue;
        }

        do_scan();
        int order[WIFI_MAX_NETS];
        int n = candidate_order(&nets, order);
        const wifi_net_t *cur = NULL;
        for (int k = 0; k < n && !cur; k++) {
            if (try_connect(&nets.net[order[k]])) cur = &nets.net[order[k]];
        }

        if (cur) {
            backoff = 0;
            /* stay until the link drops, or the list changed and our network is no longer in it */
            for (;;) {
                EventBits_t b = xEventGroupWaitBits(s_ev, BIT_DISC | BIT_CHANGED, pdFALSE, pdFALSE, portMAX_DELAY);
                if (b & BIT_DISC) {
                    ESP_LOGW(TAG, "connection to '%s' lost", s_sta_ssid);
                    break;
                }
                xEventGroupClearBits(s_ev, BIT_CHANGED);
                bool still = false;
                xSemaphoreTake(s_nets_lock, portMAX_DELAY);
                for (int i = 0; i < s_nets.count && !still; i++) {
                    still = strcmp(s_nets.net[i].ssid, cur->ssid) == 0 && strcmp(s_nets.net[i].pass, cur->pass) == 0;
                }
                xSemaphoreGive(s_nets_lock);
                if (!still) {
                    ESP_LOGI(TAG, "'%s' removed or changed, reconnecting", s_sta_ssid);
                    esp_wifi_disconnect();
                    xEventGroupWaitBits(s_ev, BIT_DISC, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        s_sta_ssid[0] = 0;
        int delay_s = 1 << (backoff < 6 ? backoff : 6);
        if (delay_s > 60) delay_s = 60;
        backoff++;
        ESP_LOGW(TAG, "no saved network reachable, retry in %d s", delay_s);
        xEventGroupWaitBits(s_ev, BIT_CHANGED, pdFALSE, pdFALSE, pdMS_TO_TICKS(delay_s * 1000));
    }
}

static void mdns_start(const char *hostname)
{
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set(hostname);
    mdns_instance_name_set("MacroPad 3");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local", hostname);
}

esp_err_t wifi_mgr_start_sta(const wifi_list_t *nets, const char *hostname)
{
    common_init();
    s_nets = *nets;
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, hostname);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* modem sleep is mandatory when Wi-Fi and Bluetooth share the radio */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    mdns_start(hostname);
    ESP_LOGI(TAG, "%d saved network(s)", nets->count);
    xTaskCreatePinnedToCore(sta_task, "wifi_sta", 4096, NULL, 4, NULL, 0);
    return ESP_OK;
}

void wifi_mgr_set_networks(const wifi_list_t *nets)
{
    if (s_ap_mode || !s_nets_lock) return;
    xSemaphoreTake(s_nets_lock, portMAX_DELAY);
    s_nets = *nets;
    xSemaphoreGive(s_nets_lock);
    xEventGroupSetBits(s_ev, BIT_CHANGED);
}

esp_err_t wifi_mgr_start_ap(void)
{
    s_ap_mode = true;
    common_init();
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "MacroPad-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    strcpy(s_ip, "192.168.4.1");
    ESP_LOGI(TAG, "setup AP '%s' at http://192.168.4.1", s_ap_ssid);
    return ESP_OK;
}

bool wifi_mgr_connected(void) { return s_connected; }
bool wifi_mgr_ap_mode(void) { return s_ap_mode; }
void wifi_mgr_get_ip(char *buf, size_t len) { strlcpy(buf, s_ip, len); }
const char *wifi_mgr_ap_ssid(void) { return s_ap_ssid; }
const char *wifi_mgr_sta_ssid(void) { return s_sta_ssid; }

int wifi_mgr_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

char *wifi_mgr_scan_json(void)
{
    if (!s_scan_lock) return NULL;
    do_scan();
    cJSON *arr = cJSON_CreateArray();
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    for (int i = 0; i < s_scan_n; i++) {
        const char *ssid = (const char *)s_scan[i].ssid;
        if (!ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < i && !dup; j++) dup = strcmp(ssid, (const char *)s_scan[j].ssid) == 0;
        if (dup) continue; /* records are sorted by RSSI; keep the strongest */
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", ssid);
        cJSON_AddNumberToObject(o, "rssi", s_scan[i].rssi);
        cJSON_AddBoolToObject(o, "secure", s_scan[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    xSemaphoreGive(s_scan_lock);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}
