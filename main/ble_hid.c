#include "ble_hid.h"
#include "hid_out.h"
#include "latency.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void);

static const char *TAG = "ble";

#define APPEARANCE_KEYBOARD 0x03C1
#define ADV_ITVL_MIN        32  /* x0.625 ms = 20 ms: fast (re)connection */
#define ADV_ITVL_MAX        48  /* 30 ms */
/* Requested connection parameters: 7.5..15 ms interval, no slave latency (lowest latency the
 * spec allows; the host has the final word, e.g. Apple uses >= 11.25 ms for HID). */
#define CONN_ITVL_MIN       6   /* x1.25 ms */
#define CONN_ITVL_MAX       12
#define CONN_SUP_TIMEOUT    300 /* x10 ms */

enum {
    A_MANUF, A_PNP, A_BATT, A_HID_INFO, A_REPORT_MAP, A_CTRL_POINT, A_PROTO,
    A_KB_IN, A_KB_OUT, A_CC_IN, A_SYS_IN, A_BOOT_KB_IN, A_BOOT_KB_OUT,
};

static uint16_t h_kb_in, h_cc_in, h_sys_in, h_boot_kb_in;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_encrypted;
static volatile bool s_sub_kb, s_sub_cc, s_sub_sys, s_sub_boot;
static volatile bool s_enabled;
static volatile bool s_synced;
static volatile uint32_t s_itvl_us;
static uint8_t s_own_addr_type;
static uint8_t s_proto = 1; /* 0 = boot, 1 = report protocol */
static uint8_t s_kb_in[8];
static uint16_t s_cc_in;
static uint8_t s_sys_in;
static uint8_t s_leds;
static const uint8_t s_batt = 100;
static char s_name[32] = "MacroPad 3";

static const uint8_t s_pnp[7] = { 0x02, 0x3A, 0x30, 0x04, 0x40, 0x00, 0x03 }; /* USB VID 303A PID 4004 v3.0 */
static const uint8_t s_hid_info[4] = { 0x11, 0x01, 0x00, 0x03 }; /* HID 1.11, no country, wake + connectable */
static const uint8_t rr_kb_in[2] = { RID_KEYBOARD, 1 };
static const uint8_t rr_kb_out[2] = { RID_KEYBOARD, 2 };
static const uint8_t rr_cc_in[2] = { RID_CONSUMER, 1 };
static const uint8_t rr_sys_in[2] = { RID_SYSTEM, 1 };

static void adv_start(void);

/* ---------- GATT ---------- */

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const int id = (int)(intptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const void *p;
        size_t n;
        switch (id) {
        case A_MANUF:       p = "MacroPad"; n = 8; break;
        case A_PNP:         p = s_pnp; n = sizeof(s_pnp); break;
        case A_BATT:        p = &s_batt; n = 1; break;
        case A_HID_INFO:    p = s_hid_info; n = sizeof(s_hid_info); break;
        case A_REPORT_MAP:  p = hid_report_map; n = hid_report_map_len; break;
        case A_PROTO:       p = &s_proto; n = 1; break;
        case A_KB_IN:
        case A_BOOT_KB_IN:  p = s_kb_in; n = sizeof(s_kb_in); break;
        case A_KB_OUT:
        case A_BOOT_KB_OUT: p = &s_leds; n = 1; break;
        case A_CC_IN:       p = &s_cc_in; n = 2; break;
        case A_SYS_IN:      p = &s_sys_in; n = 1; break;
        default:            return BLE_ATT_ERR_READ_NOT_PERMITTED;
        }
        return os_mbuf_append(ctxt->om, p, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[8];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0 || len < 1) return 0;
        if (id == A_PROTO) {
            s_proto = buf[0] ? 1 : 0;
            ESP_LOGI(TAG, "%s protocol", s_proto ? "report" : "boot");
        } else if (id == A_KB_OUT || id == A_BOOT_KB_OUT) {
            s_leds = buf[0];
        } /* control point (suspend / exit suspend) needs no action */
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int dsc_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_DSC) return BLE_ATT_ERR_UNLIKELY;
    return os_mbuf_append(ctxt->om, arg, 2) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

#define CHR(uuid16, id, fl) { .uuid = BLE_UUID16_DECLARE(uuid16), .access_cb = chr_access, \
                              .arg = (void *)(intptr_t)(id), .flags = (fl) }
#define REPORT_REF(rr) (struct ble_gatt_dsc_def[]) { \
        { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ, .access_cb = dsc_access, \
          .arg = (void *)(rr) }, { 0 } }

#define F_RD      BLE_GATT_CHR_F_READ
#define F_RD_ENC  (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC)
#define F_WR_ENC  (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_NO_RSP)

static const struct ble_gatt_svc_def s_svcs[] = {
    {   /* Device Information */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]) {
            CHR(0x2A29, A_MANUF, F_RD),
            CHR(0x2A50, A_PNP, F_RD),
            { 0 },
        },
    },
    {   /* Battery (hosts like to see one; reports a constant 100 %) */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]) {
            CHR(0x2A19, A_BATT, F_RD | BLE_GATT_CHR_F_NOTIFY),
            { 0 },
        },
    },
    {   /* Human Interface Device */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]) {
            CHR(0x2A4A, A_HID_INFO, F_RD),
            CHR(0x2A4B, A_REPORT_MAP, F_RD_ENC),
            CHR(0x2A4C, A_CTRL_POINT, BLE_GATT_CHR_F_WRITE_NO_RSP),
            CHR(0x2A4E, A_PROTO, F_RD | BLE_GATT_CHR_F_WRITE_NO_RSP),
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = chr_access, .arg = (void *)A_KB_IN,
                .flags = F_RD_ENC | BLE_GATT_CHR_F_NOTIFY, .val_handle = &h_kb_in,
                .descriptors = REPORT_REF(rr_kb_in),
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = chr_access, .arg = (void *)A_KB_OUT,
                .flags = F_RD_ENC | F_WR_ENC,
                .descriptors = REPORT_REF(rr_kb_out),
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = chr_access, .arg = (void *)A_CC_IN,
                .flags = F_RD_ENC | BLE_GATT_CHR_F_NOTIFY, .val_handle = &h_cc_in,
                .descriptors = REPORT_REF(rr_cc_in),
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = chr_access, .arg = (void *)A_SYS_IN,
                .flags = F_RD_ENC | BLE_GATT_CHR_F_NOTIFY, .val_handle = &h_sys_in,
                .descriptors = REPORT_REF(rr_sys_in),
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A22), .access_cb = chr_access, .arg = (void *)A_BOOT_KB_IN,
                .flags = F_RD_ENC | BLE_GATT_CHR_F_NOTIFY, .val_handle = &h_boot_kb_in,
            },
            CHR(0x2A32, A_BOOT_KB_OUT, F_RD_ENC | F_WR_ENC),
            { 0 },
        },
    },
    { 0 },
};

/* ---------- connection ---------- */

static void request_fast_params(uint16_t conn)
{
    struct ble_gap_upd_params p = {
        .itvl_min = CONN_ITVL_MIN,
        .itvl_max = CONN_ITVL_MAX,
        .latency = 0,
        .supervision_timeout = CONN_SUP_TIMEOUT,
    };
    int rc = ble_gap_update_params(conn, &p);
    if (rc) ESP_LOGW(TAG, "conn param update request failed: %d", rc);
}

static void update_itvl(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) == 0) {
        s_itvl_us = d.conn_itvl * 1250;
        ESP_LOGI(TAG, "conn interval %.2f ms, latency %d", d.conn_itvl * 1.25, d.conn_latency);
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_encrypted = false;
            s_sub_kb = s_sub_cc = s_sub_sys = s_sub_boot = false;
            ESP_LOGI(TAG, "connected");
            if (!s_enabled) {
                ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
                break;
            }
            update_itvl(s_conn);
            ble_gap_security_initiate(s_conn); /* bonded hosts re-encrypt right away */
        } else if (s_enabled) {
            adv_start();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason 0x%03x)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_encrypted = false;
        s_sub_kb = s_sub_cc = s_sub_sys = s_sub_boot = false;
        s_itvl_us = 0;
        if (s_enabled) adv_start();
        break;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc d;
        if (ev->enc_change.status == 0 && ble_gap_conn_find(ev->enc_change.conn_handle, &d) == 0) {
            s_encrypted = d.sec_state.encrypted;
            ESP_LOGI(TAG, "encrypted (bonded %d)", d.sec_state.bonded);
            request_fast_params(ev->enc_change.conn_handle);
        } else {
            ESP_LOGW(TAG, "encryption failed: %d", ev->enc_change.status);
        }
        break;
    }

    case BLE_GAP_EVENT_SUBSCRIBE: {
        const uint16_t h = ev->subscribe.attr_handle;
        const bool on = ev->subscribe.cur_notify;
        if (h == h_kb_in) s_sub_kb = on;
        else if (h == h_cc_in) s_sub_cc = on;
        else if (h == h_sys_in) s_sub_sys = on;
        else if (h == h_boot_kb_in) s_sub_boot = on;
        break;
    }

    case BLE_GAP_EVENT_CONN_UPDATE:
        update_itvl(ev->conn_update.conn_handle);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_enabled && s_conn == BLE_HS_CONN_HANDLE_NONE) adv_start();
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* host lost its keys (re-paired): drop the old bond and accept the new pairing */
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", ev->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

static void adv_start(void)
{
    if (!s_synced || !s_enabled || ble_gap_adv_active() || s_conn != BLE_HS_CONN_HANDLE_NONE) return;

    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.appearance = APPEARANCE_KEYBOARD;
    f.appearance_is_present = 1;
    f.uuids16 = (ble_uuid16_t[]) { BLE_UUID16_INIT(0x1812) };
    f.num_uuids16 = 1;
    f.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) { ESP_LOGE(TAG, "adv fields: %d", rc); return; }

    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = strlen(s_name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = ADV_ITVL_MIN,
        .itvl_max = ADV_ITVL_MAX,
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc) ESP_LOGE(TAG, "adv start: %d", rc);
    else ESP_LOGI(TAG, "advertising as '%s'", s_name);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_synced = true;
    adv_start();
}

static void on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "host reset: %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---------- API ---------- */

void ble_hid_init(const char *name)
{
    strlcpy(s_name, name, sizeof(s_name));
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO; /* "just works" pairing, no PIN */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_svcs) == 0 ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_svcs) == 0 ? ESP_OK : ESP_FAIL);
    ble_svc_gap_device_name_set(s_name);
    ble_svc_gap_device_appearance_set(APPEARANCE_KEYBOARD);
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}

void ble_hid_set_enabled(bool en)
{
    s_enabled = en;
    if (!s_synced) return; /* on_sync() advertises if enabled */
    if (en) {
        adv_start();
    } else {
        if (ble_gap_adv_active()) ble_gap_adv_stop();
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void ble_hid_set_name(const char *name)
{
    if (strcmp(name, s_name) == 0) return;
    strlcpy(s_name, name, sizeof(s_name));
    ble_svc_gap_device_name_set(s_name);
    if (s_synced && ble_gap_adv_active()) {
        ble_gap_adv_stop();
        adv_start();
    }
}

bool ble_hid_connected(void)
{
    return s_enabled && s_conn != BLE_HS_CONN_HANDLE_NONE && s_encrypted && (s_sub_kb || s_sub_boot || s_sub_cc);
}

uint32_t ble_hid_conn_interval_us(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE ? s_itvl_us : 0; }

/* Notifications are queued in order inside the stack; several can go out in one connection event.
 * When the stack runs out of buffers, wait a moment and retry so no report is lost. */
static bool notify(uint16_t handle, const void *data, uint16_t len)
{
    for (int tries = 0; tries < 50; tries++) {
        uint16_t conn = s_conn;
        if (conn == BLE_HS_CONN_HANDLE_NONE || !s_encrypted) return false;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        if (om) {
            int rc = ble_gatts_notify_custom(conn, handle, om); /* consumes om */
            if (rc == 0) {
                uint32_t t0 = lat_take_mark();
                if (t0) lat_add(true, lat_now_us() - t0);
                return true;
            }
            if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) {
                ESP_LOGW(TAG, "notify failed: %d", rc);
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

bool ble_hid_send_keyboard(const uint8_t report[8])
{
    memcpy(s_kb_in, report, 8);
    if (s_proto == 0) return s_sub_boot ? notify(h_boot_kb_in, report, 8) : false;
    return s_sub_kb ? notify(h_kb_in, report, 8) : false;
}

bool ble_hid_send_system(uint8_t value)
{
    s_sys_in = value;
    return s_sub_sys ? notify(h_sys_in, &value, 1) : false;
}

bool ble_hid_send_consumer(uint16_t usage)
{
    s_cc_in = usage;
    return s_sub_cc ? notify(h_cc_in, &usage, 2) : false;
}

void ble_hid_forget_bonds(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    ble_store_clear();
    ESP_LOGI(TAG, "all pairings deleted");
}
