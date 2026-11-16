#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "config.h"

/* Station mode: connect to the strongest saved network in range and keep reconnecting. Saved
 * networks not seen in the scan (e.g. hidden ones) are tried last. */
esp_err_t wifi_mgr_start_sta(const wifi_list_t *nets, const char *hostname);
/* Replace the saved networks at runtime (station mode only; reconnects if needed). */
void wifi_mgr_set_networks(const wifi_list_t *nets);
/* Setup mode: open access point "MacroPad-XXXX" (APSTA so networks can be scanned). */
esp_err_t wifi_mgr_start_ap(void);

bool wifi_mgr_connected(void);
bool wifi_mgr_ap_mode(void);
void wifi_mgr_get_ip(char *buf, size_t len);
int wifi_mgr_rssi(void);
const char *wifi_mgr_ap_ssid(void);
/* SSID currently connected to (or being tried); "" if none. */
const char *wifi_mgr_sta_ssid(void);

/* Blocking scan (~2 s). Returns a JSON array [{"ssid":..,"rssi":..,"secure":..}] (caller frees). */
char *wifi_mgr_scan_json(void);
