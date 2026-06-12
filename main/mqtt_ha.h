/**
 * MQTT & Home Assistant Module
 *
 * Handles MQTT client, HA auto-discovery, command handling
 */

#ifndef MQTT_HA_H
#define MQTT_HA_H

#include "mqtt_client.h"
#include "esp_http_client.h"

// ==================== MQTT Configuration ====================
#define MQTT_LOG_TOPIC     "home/gateway/log"

// ==================== OTA Configuration ====================
#define DEFAULT_FW_URL     "http://your-server/ota/firmware.bin"
#define FW_BASE_URL       "http://your-server/ota"

// ==================== Function Declarations ====================

/**
 * Start MQTT client
 */
void mqtt_app_start(void);

/**
 * MQTT event handler
 */
void mqtt_event_handler(void *arg, esp_event_base_t event_base,
                       int32_t event_id, void *event_data);

/**
 * Handle MQTT message
 */
void handle_mqtt_message(esp_mqtt_event_handle_t event);

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
 * Send log sensor discovery
 */
void send_log_sensor_discovery(void);

/**
 * Subscribe to MQTT topics
 */
void subscribe_all(void);

/**
 * Handle gateway command
 */
void handle_gateway_command(const char *payload, int len);

/**
 * Handle OTA command
 */
void handle_ota_command(const char *payload, int len);

/**
 * Handle 1527 mapping command
 */
void handle_1527map_command(const char *payload, int len);

/**
 * Send mapping table to STC15W
 */
void stc15_send_map_set(const uint8_t *entries, int count);

/**
 * Publish 1527 mapping response to MQTT
 */
void publish_1527map_resp(const uint8_t *entries, int count);

/**
 * Start OTA update
 */
void start_ota_update(const char *url);

#endif // MQTT_HA_H
