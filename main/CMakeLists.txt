idf_component_register(SRCS "wifi_prov.c" "mqtt_ha.c" "gateway_state.c" "uart_protocol.c" "esp32_gateway_main.c"
                    PRIV_REQUIRES esp_wifi esp_event nvs_flash mqtt json driver esp_http_client esp_https_ota esp_http_server esp_netif esp_timer lwip app_update
                    INCLUDE_DIRS ".")
