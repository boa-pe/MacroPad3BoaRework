#include "web_server.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "cJSON.h"
#include "config.h"
#include "keyproc.h"
#include "scanner.h"
#include "hid_out.h"
#include "ble_hid.h"
#include "wifi_mgr.h"
#include "status_led.h"
#include "auth.h"
#include "latency.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

#define MAX_BODY 6144

/* ---------- helpers ---------- */

static char *read_body(httpd_req_t *req)
{
    if (req->content_len >= MAX_BODY) return NULL;
    char *buf = malloc(req->content_len + 1);
    if (!buf) return NULL;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(buf); return NULL; }
        got += r;
    }
    buf[got] = 0;
    return buf;
}

static cJSON *read_json(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return NULL;
    cJSON *j = cJSON_Parse(body);
    free(body);
    return j;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    return send_json(req, o);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static void reboot_soon(void)
{
    config_flush();
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
}

static int get_int(const cJSON *o, const char *name, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, name);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static bool get_bool(const cJSON *o, const char *name, bool def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, name);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static void get_str(const cJSON *o, const char *name, char *dst, size_t len)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, name);
    if (cJSON_IsString(v)) strlcpy(dst, v->valuestring, len);
}

/* ---------- config <-> JSON ---------- */

static cJSON *action_json(const key_action_t *a)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "t", a->type);
    cJSON_AddNumberToObject(o, "m", a->mods);
    cJSON_AddNumberToObject(o, "k", a->key);
    cJSON_AddNumberToObject(o, "u", a->usage);
    cJSON_AddStringToObject(o, "s", a->text);
    return o;
}

static void action_parse(const cJSON *o, key_action_t *a)
{
    memset(a, 0, sizeof(*a));
    if (!cJSON_IsObject(o)) return;
    a->type = get_int(o, "t", ACT_NONE);
    a->mods = get_int(o, "m", 0);
    a->key = get_int(o, "k", 0);
    a->usage = get_int(o, "u", 0);
    get_str(o, "s", a->text, sizeof(a->text));
}

static cJSON *profile_json(const profile_t *p)
{
    cJSON *o = cJSON_CreateObject();
    char col[8];
    snprintf(col, sizeof(col), "#%02x%02x%02x", p->color[0], p->color[1], p->color[2]);
    cJSON_AddStringToObject(o, "name", p->name);
    cJSON_AddStringToObject(o, "color", col);
    cJSON_AddNumberToObject(o, "scan_khz", p->scan_khz);
    cJSON *keys = cJSON_AddArrayToObject(o, "keys");
    for (int k = 0; k < NUM_KEYS; k++) cJSON_AddItemToArray(keys, action_json(&p->keys[k]));
    return o;
}

static void profile_parse(const cJSON *o, profile_t *p)
{
    get_str(o, "name", p->name, sizeof(p->name));
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(o, "color");
    if (cJSON_IsString(c) && c->valuestring[0] == '#' && strlen(c->valuestring) == 7) {
        unsigned v = strtoul(c->valuestring + 1, NULL, 16);
        p->color[0] = v >> 16;
        p->color[1] = v >> 8;
        p->color[2] = v;
    }
    p->scan_khz = get_int(o, "scan_khz", p->scan_khz);
    const cJSON *keys = cJSON_GetObjectItemCaseSensitive(o, "keys");
    if (cJSON_IsArray(keys)) {
        for (int k = 0; k < NUM_KEYS && k < cJSON_GetArraySize(keys); k++) {
            action_parse(cJSON_GetArrayItem(keys, k), &p->keys[k]);
        }
    }
}

/* saved networks; passwords never leave the device, only whether one is set */
static cJSON *wifi_json(void)
{
    wifi_list_t l;
    config_get_wifi(&l);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < l.count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", l.net[i].ssid);
        cJSON_AddBoolToObject(o, "has_pass", l.net[i].pass[0] != 0);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static cJSON *global_json(const global_cfg_t *g)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "active", g->active);
    cJSON_AddNumberToObject(o, "debounce_ms", g->debounce_ms);
    cJSON_AddNumberToObject(o, "brightness", g->brightness);
    cJSON_AddNumberToObject(o, "enc_steps", g->enc_steps);
    cJSON_AddBoolToObject(o, "enc_reverse", g->enc_reverse);
    cJSON_AddBoolToObject(o, "wifi_enabled", g->wifi_enabled);
    cJSON_AddStringToObject(o, "ble_name", g->ble_name);
    cJSON_AddStringToObject(o, "hostname", g->hostname);
    cJSON_AddItemToObject(o, "wifi_networks", wifi_json());
    return o;
}

/* ---------- handlers ---------- */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_get_config(httpd_req_t *req)
{
    global_cfg_t g;
    config_get_global(&g);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "global", global_json(&g));
    cJSON *arr = cJSON_AddArrayToObject(root, "profiles");
    profile_t *p = malloc(sizeof(profile_t));
    if (!p) { cJSON_Delete(root); return send_error(req, "500 Internal Server Error", "no memory"); }
    for (int i = 0; i < NUM_PROFILES; i++) {
        config_get_profile(i, p);
        cJSON_AddItemToArray(arr, profile_json(p));
    }
    free(p);
    return send_json(req, root);
}

/* POST /api/profile?i=N  body: profile object (fields missing keep their value) */
static esp_err_t h_set_profile(httpd_req_t *req)
{
    char q[16], v[4];
    int idx = -1;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "i", v, sizeof(v)) == ESP_OK) {
        idx = atoi(v);
    }
    if (idx < 0 || idx >= NUM_PROFILES) return send_error(req, "400 Bad Request", "bad profile index");
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON");
    profile_t *p = malloc(sizeof(profile_t));
    if (!p) { cJSON_Delete(j); return send_error(req, "500 Internal Server Error", "no memory"); }
    if (get_bool(j, "reset", false)) config_default_profile(idx, p);
    else {
        config_get_profile(idx, p);
        profile_parse(j, p);
    }
    cJSON_Delete(j);
    config_set_profile(idx, p);
    config_get_profile(idx, p);
    cJSON *out = profile_json(p);
    free(p);
    return send_json(req, out);
}

static esp_err_t h_set_global(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON");
    global_cfg_t g;
    config_get_global(&g);
    g.debounce_ms = get_int(j, "debounce_ms", g.debounce_ms);
    g.brightness = get_int(j, "brightness", g.brightness);
    g.enc_steps = get_int(j, "enc_steps", g.enc_steps);
    g.enc_reverse = get_bool(j, "enc_reverse", g.enc_reverse);
    g.wifi_enabled = get_bool(j, "wifi_enabled", g.wifi_enabled);
    g.active = get_int(j, "active", g.active);
    get_str(j, "ble_name", g.ble_name, sizeof(g.ble_name));
    get_str(j, "hostname", g.hostname, sizeof(g.hostname));
    cJSON_Delete(j);
    config_set_global(&g);
    config_get_global(&g);
    return send_json(req, global_json(&g));
}

static esp_err_t h_set_active(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON");
    int idx = get_int(j, "active", -1);
    cJSON_Delete(j);
    if (config_set_active(idx) != ESP_OK) return send_error(req, "400 Bad Request", "bad profile index");
    return send_ok(req);
}

static esp_err_t h_status(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(o, "fw", app->version);
    hid_link_t link = hid_out_link();
    cJSON_AddStringToObject(o, "link", link == LINK_USB ? "usb" : link == LINK_BLE ? "ble" : "none");
    cJSON_AddBoolToObject(o, "usb", hid_out_usb_mounted());
    cJSON_AddBoolToObject(o, "ble_enabled", hid_out_ble_enabled());
    cJSON_AddBoolToObject(o, "ble", hid_out_ble_connected());
    cJSON_AddNumberToObject(o, "ble_itvl_us", ble_hid_conn_interval_us());
    /* estimated key-to-host latency for the active link */
    if (link != LINK_NONE) {
        lat_estimate_t e;
        lat_estimate(link == LINK_BLE, scanner_rate(), ble_hid_conn_interval_us(), &e);
        cJSON *l = cJSON_AddObjectToObject(o, "lat");
        cJSON_AddBoolToObject(l, "measured", e.measured);
        cJSON_AddNumberToObject(l, "n", e.n);
        cJSON_AddNumberToObject(l, "avg_us", e.avg_us);
        cJSON_AddNumberToObject(l, "max_us", e.max_us);
        cJSON_AddNumberToObject(l, "dev_avg_us", e.dev_avg_us);
        cJSON_AddNumberToObject(l, "dev_max_us", e.dev_max_us);
    }
    cJSON_AddNumberToObject(o, "active", config_active());
    cJSON_AddBoolToObject(o, "selecting", keyproc_selecting());
    cJSON_AddNumberToObject(o, "pending", keyproc_pending_profile());
    cJSON_AddNumberToObject(o, "pressed", keyproc_pressed_mask());
    cJSON_AddNumberToObject(o, "scan_khz", scanner_rate());
    uint32_t ccw, cw;
    scanner_enc_counts(&ccw, &cw);
    cJSON_AddNumberToObject(o, "enc_ccw", ccw);
    cJSON_AddNumberToObject(o, "enc_cw", cw);
    char ip[16];
    wifi_mgr_get_ip(ip, sizeof(ip));
    cJSON_AddBoolToObject(o, "ap", wifi_mgr_ap_mode());
    cJSON_AddStringToObject(o, "ap_ssid", wifi_mgr_ap_ssid());
    cJSON_AddStringToObject(o, "ip", ip);
    cJSON_AddStringToObject(o, "ssid", wifi_mgr_ap_mode() ? "" : wifi_mgr_sta_ssid());
    cJSON_AddNumberToObject(o, "rssi", wifi_mgr_rssi());
    cJSON_AddNumberToObject(o, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return send_json(req, o);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    char *s = wifi_mgr_scan_json();
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, s ? s : "[]");
    free(s);
    return err;
}

/* POST /api/wifi {"networks":[{"ssid":"..","pass":".."}, ...]}  (max 5)
 * Replaces the saved list. A network sent without "pass" keeps its stored password.
 * In station mode the change applies live; from setup mode (or with Wi-Fi disabled) it reboots. */
static esp_err_t h_wifi(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON");
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, "networks");
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) > WIFI_MAX_NETS) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", "expected up to 5 networks");
    }
    wifi_list_t old, l = { 0 };
    config_get_wifi(&old);
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(it, "ssid");
        const cJSON *pass = cJSON_GetObjectItemCaseSensitive(it, "pass");
        if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) continue;
        if (strlen(ssid->valuestring) > 32 || (cJSON_IsString(pass) && strlen(pass->valuestring) > 64)) {
            cJSON_Delete(j);
            return send_error(req, "400 Bad Request", "SSID max 32, password max 64 characters");
        }
        wifi_net_t *n = &l.net[l.count++];
        strlcpy(n->ssid, ssid->valuestring, sizeof(n->ssid));
        if (cJSON_IsString(pass)) {
            strlcpy(n->pass, pass->valuestring, sizeof(n->pass));
        } else {
            for (int i = 0; i < old.count; i++) {
                if (strcmp(old.net[i].ssid, n->ssid) == 0) strlcpy(n->pass, old.net[i].pass, sizeof(n->pass));
            }
        }
    }
    cJSON_Delete(j);
    config_set_wifi(&l);

    global_cfg_t g;
    config_get_global(&g);
    bool reboot = l.count > 0 && (wifi_mgr_ap_mode() || !g.wifi_enabled);
    if (reboot && !g.wifi_enabled) {
        g.wifi_enabled = true;
        config_set_global(&g);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "wifi_networks", wifi_json());
    cJSON_AddBoolToObject(o, "reboot", reboot);
    send_json(req, o);
    if (reboot) reboot_soon();
    return ESP_OK;
}

static esp_err_t h_ble_forget(httpd_req_t *req)
{
    ble_hid_forget_bonds();
    return send_ok(req);
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    send_ok(req);
    reboot_soon();
    return ESP_OK;
}

static esp_err_t h_factory(httpd_req_t *req)
{
    config_factory_reset();
    ble_hid_forget_bonds();
    send_ok(req);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL); /* no flush: keep NVS empty */
    return ESP_OK;
}

/* POST /api/ota  body: raw application .bin */
static esp_err_t h_ota(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return send_error(req, "500 Internal Server Error", "no OTA partition");
    esp_ota_handle_t ota;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "OTA begin failed");
    }
    status_led_set_ota(true);
    ESP_LOGI(TAG, "OTA: %d bytes -> %s", req->content_len, part->label);
    char *buf = malloc(4096);
    size_t remaining = req->content_len;
    esp_err_t err = buf ? ESP_OK : ESP_ERR_NO_MEM;
    while (err == ESP_OK && remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { err = ESP_FAIL; break; }
        err = esp_ota_write(ota, buf, r);
        remaining -= r;
    }
    free(buf);
    if (err == ESP_OK) err = esp_ota_end(ota);
    else esp_ota_abort(ota);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
    status_led_set_ota(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        return send_error(req, "400 Bad Request", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "OTA done, rebooting");
    send_ok(req);
    reboot_soon();
    return ESP_OK;
}

/* ---------- physical-presence verification ---------- */

#define SESSION_COOKIE "mp_session"

static bool get_session_token(httpd_req_t *req, char *tok, size_t len)
{
    return httpd_req_get_cookie_val(req, SESSION_COOKIE, tok, &len) == ESP_OK;
}

static bool authed_ex(httpd_req_t *req, bool touch)
{
    char tok[AUTH_TOKEN_LEN + 2];
    return get_session_token(req, tok, sizeof(tok)) && auth_session_valid(tok, touch);
}

static bool authed(httpd_req_t *req) { return authed_ex(req, false); }

typedef esp_err_t (*handler_fn_t)(httpd_req_t *req);

/* every API route except the verification ones goes through here; user actions restart the
 * idle timer, background polls (/api/status) don't */
static esp_err_t guarded(httpd_req_t *req)
{
    if (!authed_ex(req, true)) return send_error(req, "401 Unauthorized", "verification required");
    return ((handler_fn_t)req->user_ctx)(req);
}

static esp_err_t guarded_passive(httpd_req_t *req)
{
    if (!authed_ex(req, false)) return send_error(req, "401 Unauthorized", "verification required");
    return ((handler_fn_t)req->user_ctx)(req);
}

/* POST /api/ping: the page reports user activity (clicks, typing) that caused no other request */
static esp_err_t h_ping(httpd_req_t *req) { return send_ok(req); }

static bool query_id(httpd_req_t *req, char id[AUTH_ID_LEN + 1])
{
    char q[64];
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
           httpd_query_key_value(q, "id", id, AUTH_ID_LEN + 1) == ESP_OK;
}

static esp_err_t h_session(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "authed", authed(req));
    return send_json(req, o);
}

/* POST /api/auth/start -> {"id":"..","keys":[k1,k2,k3,k4],"ttl":60}  (keys are 0-based) */
static esp_err_t h_auth_start(httpd_req_t *req)
{
    char id[AUTH_ID_LEN + 1];
    uint8_t keys[AUTH_SEQ_LEN];
    int ttl;
    if (auth_start(id, keys, &ttl) != ESP_OK) return send_error(req, "429 Too Many Requests", "wait a few seconds");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", id);
    cJSON *arr = cJSON_AddArrayToObject(o, "keys");
    for (int i = 0; i < AUTH_SEQ_LEN; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(keys[i]));
    cJSON_AddNumberToObject(o, "ttl", ttl);
    return send_json(req, o);
}

/* GET /api/auth/status?id=.. -> {"state":"pending|ok|failed|expired|cancelled|none","progress":n,"ttl":s}
 * On "ok" the session cookie is set (once). */
static esp_err_t h_auth_status(httpd_req_t *req)
{
    static const char *names[] = { "none", "pending", "ok", "failed", "expired", "cancelled" };
    char id[AUTH_ID_LEN + 1], token[AUTH_TOKEN_LEN + 1];
    int progress = 0, ttl = 0;
    auth_state_t st = query_id(req, id) ? auth_status(id, &progress, &ttl, token) : AUTH_ST_NONE;
    if (st == AUTH_ST_NONE) token[0] = 0;
    char cookie[AUTH_TOKEN_LEN + 64];
    if (st == AUTH_ST_OK && token[0]) {
        snprintf(cookie, sizeof(cookie), SESSION_COOKIE "=%s; Path=/; HttpOnly; SameSite=Strict", token);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", names[st]);
    cJSON_AddNumberToObject(o, "progress", progress);
    cJSON_AddNumberToObject(o, "ttl", ttl);
    return send_json(req, o); /* cookie buffer is still in scope here */
}

static esp_err_t h_auth_cancel(httpd_req_t *req)
{
    char id[AUTH_ID_LEN + 1];
    if (query_id(req, id)) auth_cancel_id(id);
    return send_ok(req);
}

static esp_err_t h_logout(httpd_req_t *req)
{
    char tok[AUTH_TOKEN_LEN + 2];
    if (get_session_token(req, tok, sizeof(tok))) auth_logout(tok);
    httpd_resp_set_hdr(req, "Set-Cookie", SESSION_COOKIE "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return send_ok(req);
}

static esp_err_t h_captive(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t web_server_start(bool setup_mode)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    cfg.stack_size = 6144;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    cfg.core_id = 0;
    cfg.recv_wait_timeout = 10;
    httpd_handle_t h;
    esp_err_t err = httpd_start(&h, &cfg);
    if (err != ESP_OK) return err;

#define REG(path, m, fn) do { httpd_uri_t u = { .uri = path, .method = m, .handler = fn }; \
                              httpd_register_uri_handler(h, &u); } while (0)
    /* everything except the page itself (no data in it) and the verification endpoints
     * needs a verified session */
#define REG_AUTH(path, m, fn) do { httpd_uri_t u = { .uri = path, .method = m, .handler = guarded, \
                                                     .user_ctx = (void *)(fn) }; \
                                   httpd_register_uri_handler(h, &u); } while (0)
    REG("/", HTTP_GET, h_index);
    REG("/api/session", HTTP_GET, h_session);
    REG("/api/auth/start", HTTP_POST, h_auth_start);
    REG("/api/auth/status", HTTP_GET, h_auth_status);
    REG("/api/auth/cancel", HTTP_POST, h_auth_cancel);
    REG_AUTH("/api/logout", HTTP_POST, h_logout);
    REG_AUTH("/api/config", HTTP_GET, h_get_config);
    REG_AUTH("/api/profile", HTTP_POST, h_set_profile);
    REG_AUTH("/api/global", HTTP_POST, h_set_global);
    REG_AUTH("/api/active", HTTP_POST, h_set_active);
    REG_AUTH("/api/ping", HTTP_POST, h_ping);
    {
        httpd_uri_t u = { .uri = "/api/status", .method = HTTP_GET, .handler = guarded_passive, .user_ctx = (void *)h_status };
        httpd_register_uri_handler(h, &u);
    }
    REG_AUTH("/api/scan", HTTP_GET, h_scan);
    REG_AUTH("/api/wifi", HTTP_POST, h_wifi);
    REG_AUTH("/api/ble/forget", HTTP_POST, h_ble_forget);
    REG_AUTH("/api/reboot", HTTP_POST, h_reboot);
    REG_AUTH("/api/factory", HTTP_POST, h_factory);
    REG_AUTH("/api/ota", HTTP_POST, h_ota);
    if (setup_mode) REG("/*", HTTP_GET, h_captive);
#undef REG
#undef REG_AUTH
    ESP_LOGI(TAG, "web UI ready%s", setup_mode ? " (captive portal)" : "");
    return ESP_OK;
}
