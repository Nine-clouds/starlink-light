/**
 * WiFi Provisioning Module
 *
 * Handles WiFi provisioning, AP mode, HTTP config server, DNS hijacking
 */

#ifndef WIFI_PROV_H
#define WIFI_PROV_H

#include "esp_event.h"
#include "esp_http_server.h"

// ==================== WiFi Provisioning Configuration ====================
#define AP_SSID            "Cloud_Hub"
#define AP_PASSWORD        "12345678"
#define AP_MAX_CONN        4

#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "password"

// ==================== Function Declarations ====================

/**
 * Initialize WiFi
 */
void wifi_init(void);

/**
 * Start provisioning (AP mode)
 */
void start_provisioning(void);

/**
 * Switch to STA mode (after provisioning timeout)
 */
void switch_to_sta_mode(void);

/**
 * Provisioning timer callback
 */
void provisioning_timer_cb(void *arg);

/**
 * Save WiFi credentials to NVS
 */
esp_err_t save_wifi_credentials(const char *ssid, const char *password);

/**
 * Load WiFi credentials from NVS
 */
esp_err_t load_wifi_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len);

/**
 * Start config HTTP server
 */
httpd_handle_t start_config_server(void);

/**
 * Stop config HTTP server
 */
void stop_config_server(void);

/**
 * WiFi event handler
 */
void wifi_event_handler(void *arg, esp_event_base_t event_base,
                       int32_t event_id, void *event_data);

#endif // WIFI_PROV_H
