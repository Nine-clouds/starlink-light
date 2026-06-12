/**
 * Gateway State - Shared state variables across modules
 *
 * This file declares global state variables that need to be accessed
 * by multiple modules (uart_protocol, wifi_prov, mqtt_ha, etc.)
 */

#ifndef GATEWAY_STATE_H
#define GATEWAY_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include "freertos/event_groups.h"

// ==================== Room Configuration ====================
#define ROOM_COUNT         6
extern const uint8_t ROOM_IDS[ROOM_COUNT];
extern const char *ROOM_NAMES[ROOM_COUNT];
extern const char *ROOM_ENTITY_IDS[ROOM_COUNT];

// ==================== Global State ====================
extern bool room_states[ROOM_COUNT];
extern bool state_received[ROOM_COUNT];
extern esp_mqtt_client_handle_t mqtt_client;
extern bool mqtt_connected;
extern bool wifi_connected;
extern bool provisioning_active;
extern volatile bool need_provisioning;
extern volatile bool provisioning_timeout;
extern esp_timer_handle_t provisioning_timer;
extern bool ap_attempted;
extern volatile bool in_mqtt_context;
extern httpd_handle_t config_server;

extern EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

extern int s_retry_num;
extern int64_t last_heartbeat_us;
extern int64_t mqtt_connect_time_us;

// ==================== Timing Parameters ====================
#define HEARTBEAT_INTERVAL_MS   60000
#define RX_TIMEOUT_US           50000   // 50ms
#define TX_INTERVAL_US          80000   // 80ms
#define MAX_TX_WAIT_US          50000   // Max wait 50ms
#define STATE_PUBLISH_MIN_MS    300     // Min state publish interval
#define VERIFY_DELAY_US         1500000 // Verify after 1.5s
#define WIFI_MAX_RETRY          5
#define WIFI_CONNECT_TIMEOUT_MS 30000
#define PROVISIONING_TIMEOUT_S  60
#define WDT_TIMEOUT_S           10

// ==================== Query Queue State ====================
extern int query_index;
extern int64_t query_last_us;
extern int query_pass;
extern volatile bool query_pending;
extern volatile bool query_request;
extern bool verify_after_all;
extern int64_t verify_after_all_time;
#define QUERY_INTERVAL_US  200000  // 200ms

// ==================== State Publish Throttling ====================
extern int64_t last_state_publish_ms[ROOM_COUNT];

// ==================== Function Declarations ====================

/**
 * Initialize all global state variables
 */
void gateway_state_init(void);

/**
 * Publish room state to MQTT
 */
void publish_room_state(int index);

/**
 * Publish device info to MQTT
 */
void publish_device_info(void);

/**
 * Send HA auto-discovery config
 */
void send_auto_discovery_all(void);

/**
 * Subscribe to MQTT topics
 */
void subscribe_all(void);

/**
 * Handle MQTT message
 */
void handle_mqtt_message(esp_mqtt_event_handle_t event);

/**
 * Start OTA update
 */
void start_ota_update(const char *url);

#endif // GATEWAY_STATE_H
