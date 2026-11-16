idf_component_register(
    SRCS "auth.c" "latency.c" "main.c" "config.c" "scanner.c" "keyproc.c" "hid_out.c" "usb_hid.c" "ble_hid.c"
         "status_led.c" "auth.c" "wifi_mgr.c" "dns_server.c" "web_server.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html"
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer
                  esp_driver_gpio esp_driver_gptimer esp_driver_pcnt
                  bt json lwip app_update esp_app_format esp_partition
)
