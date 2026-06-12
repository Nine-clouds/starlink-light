/**
 * MQTT & Home Assistant 模块
 *
 * 负责:
 *   - MQTT客户端生命周期（连接/断开/重连）
 *   - Home Assistant自动发现配置下发
 *   - 房间状态上报与命令接收
 *   - 1527映射表管理（下发/读取）
 *   - OTA远程升级
 *   - 日志拦截：WARN/ERROR自动发MQTT，MQTT上下文内用环形缓冲延迟发送避免重入
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_netif.h"
#include "esp_https_ota.h"
#include "esp_task_wdt.h"
#include "mqtt_ha.h"
#include "gateway_state.h"
#include "uart_protocol.h"
#include "config.h"

static const char *TAG = "mqtt_ha";

// ==================== MQTT日志上报 ====================
// 机制: 通过esp_log_set_vprintf()拦截所有日志
//   - WARN/ERROR级别 → 发送到MQTT日志topic
//   - MQTT上下文内 → 写入环形缓冲，事件处理结束后延迟发送（避免重入）
//   - 非MQTT上下文 → 直接发送
#define LOG_BUF_SIZE        256
#define LOG_RING_COUNT      8
static vprintf_like_t s_original_vprintf = NULL;
static char log_ring[LOG_RING_COUNT][LOG_BUF_SIZE];
static volatile int log_ring_head = 0;
static volatile int log_ring_count = 0;

/**
 * 刷新环形缓冲中的延迟日志
 * 在MQTT事件处理结束后调用，将缓冲区中的日志一次性发送
 */
static void flush_log_buffer(void)
{
    if (!mqtt_client || !mqtt_connected) {
        log_ring_count = 0;
        return;
    }
    while (log_ring_count > 0) {
        int idx = (log_ring_head - log_ring_count + LOG_RING_COUNT) % LOG_RING_COUNT;
        esp_mqtt_client_publish(mqtt_client, MQTT_LOG_TOPIC, log_ring[idx], 0, 1, 0);
        log_ring_count--;
    }
}

/**
 * 日志拦截回调 - 替代默认vprintf
 * WARN/ERROR级别日志额外发送到MQTT，MQTT上下文内用环形缓冲延迟发送
 */
static int mqtt_log_vprintf(const char *fmt, va_list args)
{
    va_list args_copy;
    va_copy(args_copy, args);

    int ret = 0;
    if (s_original_vprintf) {
        ret = s_original_vprintf(fmt, args);
    }

    char level = fmt[0];
    if (level != 'W' && level != 'E') {
        va_end(args_copy);
        return ret;
    }

    if (!mqtt_client || !mqtt_connected) {
        va_end(args_copy);
        return ret;
    }

    char *buf;
    if (in_mqtt_context) {
        if (log_ring_count >= LOG_RING_COUNT) {
            log_ring_count--;
        }
        buf = log_ring[log_ring_head];
        log_ring_head = (log_ring_head + 1) % LOG_RING_COUNT;
        log_ring_count++;
    } else {
        static char direct_buf[LOG_BUF_SIZE];
        buf = direct_buf;
    }

    int len = vsnprintf(buf, LOG_BUF_SIZE, fmt, args_copy);
    va_end(args_copy);

    if (len <= 0 || len >= LOG_BUF_SIZE) return ret;

    if (!in_mqtt_context) {
        esp_mqtt_client_publish(mqtt_client, MQTT_LOG_TOPIC, buf, 0, 1, 0);
    }
    return ret;
}

/**
 * 初始化MQTT日志拦截
 * 注册vprintf钩子，保存原始vprintf用于串口输出
 */
void mqtt_log_init(void)
{
    s_original_vprintf = esp_log_set_vprintf(mqtt_log_vprintf);
    ESP_LOGI(TAG, "MQTT log interception enabled");
}

// ==================== MQTT事件处理 ====================

void mqtt_event_handler(void *arg, esp_event_base_t event_base,
                       int32_t event_id, void *event_data)
{
    (void)event_base;
    in_mqtt_context = true;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        mqtt_connected = true;
        mqtt_connect_time_us = esp_timer_get_time();
        last_heartbeat_us = esp_timer_get_time();
        for (int i = 0; i < ROOM_COUNT; i++) {
            char topic[64];
            snprintf(topic, sizeof(topic), "home/%s/command", ROOM_ENTITY_IDS[i]);
            esp_mqtt_client_publish(mqtt_client, topic, "", 0, 1, 1);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        subscribe_all();
        send_auto_discovery_all();
        send_log_sensor_discovery();
        publish_device_info();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        mqtt_connected = false;
        break;
    case MQTT_EVENT_DATA:
        handle_mqtt_message((esp_mqtt_event_handle_t)event_data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;
    default:
        break;
    }
    in_mqtt_context = false;
    flush_log_buffer();
}

void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
        .session.keepalive = 30,
        .network.timeout_ms = 10000,
        .network.reconnect_timeout_ms = 3000,
        .session.last_will = {
            .topic = "ota/device/status",
            .msg = "{\"status\":\"offline\"}",
            .msg_len = strlen("{\"status\":\"offline\"}"),
            .qos = 1,
            .retain = 1,
        },
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT client init failed");
        return;
    }
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    ESP_LOGI(TAG, "MQTT client started");
}

// ==================== MQTT消息处理 ====================

void handle_mqtt_message(esp_mqtt_event_handle_t event)
{
    char topic[128] = {0};
    int tlen = event->topic_len < sizeof(topic) - 1 ? event->topic_len : sizeof(topic) - 1;
    memcpy(topic, event->topic, tlen);

    if (strcmp(topic, "ota/upgrade/command") == 0) {
        handle_ota_command(event->data, event->data_len);
        return;
    }

    if (strcmp(topic, "home/gateway/cmd") == 0) {
        handle_gateway_command(event->data, event->data_len);
        return;
    }

    if (strcmp(topic, "home/gateway/1527map") == 0) {
        handle_1527map_command(event->data, event->data_len);
        return;
    }

    for (int i = 0; i < ROOM_COUNT; i++) {
        char expected[64];
        snprintf(expected, sizeof(expected), "home/%s/command", ROOM_ENTITY_IDS[i]);
        if (strcmp(topic, expected) == 0) {
            if (event->data_len >= 2 && memcmp(event->data, "ON", 2) == 0) {
                stc15_send_frame(ROOM_IDS[i], CMD_ON);
                room_states[i] = true;
                publish_room_state(i);
            } else if (event->data_len >= 3 && memcmp(event->data, "OFF", 3) == 0) {
                stc15_send_frame(ROOM_IDS[i], CMD_OFF);
                room_states[i] = false;
                publish_room_state(i);
            }
            return;
        }
    }
}

void publish_room_state(int index)
{
    if (!mqtt_connected || !mqtt_client) return;
    int64_t now = esp_timer_get_time() / 1000;
    if (now - last_state_publish_ms[index] < STATE_PUBLISH_MIN_MS) return;
    last_state_publish_ms[index] = now;

    char topic[64];
    snprintf(topic, sizeof(topic), "home/%s/state", ROOM_ENTITY_IDS[index]);
    const char *state = room_states[index] ? "ON" : "OFF";
    esp_mqtt_client_publish(mqtt_client, topic, state, 0, 1, 1);
}

void publish_device_info(void)
{
    if (!mqtt_connected || !mqtt_client) return;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", DEVICE_ID);
    cJSON_AddStringToObject(root, "version", CURRENT_VERSION);
    cJSON_AddStringToObject(root, "status", "online");
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            cJSON_AddStringToObject(root, "ip", ip_str);
        }
    }
    char *json = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(mqtt_client, "ota/device/status", json, 0, 1, 1);
    cJSON_free(json);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Device info published");
}

void send_auto_discovery_all(void)
{
    if (!mqtt_connected || !mqtt_client) return;

    for (int i = 0; i < ROOM_COUNT; i++) {
        cJSON *config = cJSON_CreateObject();
        cJSON_AddStringToObject(config, "name", ROOM_NAMES[i]);

        char cmd_topic[64], state_topic[64];
        snprintf(cmd_topic, sizeof(cmd_topic), "home/%s/command", ROOM_ENTITY_IDS[i]);
        snprintf(state_topic, sizeof(state_topic), "home/%s/state", ROOM_ENTITY_IDS[i]);

        cJSON_AddStringToObject(config, "command_topic", cmd_topic);
        cJSON_AddStringToObject(config, "state_topic", state_topic);
        cJSON_AddStringToObject(config, "payload_on", "ON");
        cJSON_AddStringToObject(config, "payload_off", "OFF");
        cJSON_AddBoolToObject(config, "optimistic", false);
        cJSON_AddNumberToObject(config, "qos", 0);
        cJSON_AddBoolToObject(config, "retain", true);
        cJSON_AddStringToObject(config, "unique_id", ROOM_ENTITY_IDS[i]);

        cJSON *device = cJSON_CreateObject();
        cJSON *identifiers = cJSON_CreateArray();
        cJSON_AddItemToArray(identifiers, cJSON_CreateString(DEVICE_ID));
        cJSON_AddItemToObject(device, "identifiers", identifiers);
        cJSON_AddStringToObject(device, "name", "星联灯控");
        cJSON_AddStringToObject(device, "manufacturer", "DIY");
        cJSON_AddStringToObject(device, "model", "SL-GW1");
        cJSON_AddItemToObject(config, "device", device);

        char disc_topic[96];
        snprintf(disc_topic, sizeof(disc_topic), "homeassistant/light/%s/config", ROOM_ENTITY_IDS[i]);

        char *json = cJSON_PrintUnformatted(config);
        esp_mqtt_client_publish(mqtt_client, disc_topic, json, 0, 1, 1);
        cJSON_free(json);
        cJSON_Delete(config);
    }
    ESP_LOGI(TAG, "Auto-discovery config sent");
}

void send_log_sensor_discovery(void)
{
    if (!mqtt_connected || !mqtt_client) return;

    cJSON *config = cJSON_CreateObject();
    cJSON_AddStringToObject(config, "name", "星联灯控日志");
    cJSON_AddStringToObject(config, "state_topic", MQTT_LOG_TOPIC);
    cJSON_AddStringToObject(config, "unique_id", DEVICE_ID "_log");

    cJSON *device = cJSON_CreateObject();
    cJSON *identifiers = cJSON_CreateArray();
    cJSON_AddItemToArray(identifiers, cJSON_CreateString(DEVICE_ID));
    cJSON_AddItemToObject(device, "identifiers", identifiers);
    cJSON_AddStringToObject(device, "name", "星联灯控");
    cJSON_AddStringToObject(device, "manufacturer", "DIY");
    cJSON_AddStringToObject(device, "model", "SL-GW1");
    cJSON_AddItemToObject(config, "device", device);

    char *json = cJSON_PrintUnformatted(config);
    esp_mqtt_client_publish(mqtt_client, "homeassistant/sensor/" DEVICE_ID "_log/config", json, 0, 1, 1);
    cJSON_free(json);
    cJSON_Delete(config);
}

void subscribe_all(void)
{
    if (!mqtt_client) return;
    for (int i = 0; i < ROOM_COUNT; i++) {
        char topic[64];
        snprintf(topic, sizeof(topic), "home/%s/command", ROOM_ENTITY_IDS[i]);
        esp_mqtt_client_subscribe(mqtt_client, topic, 0);
    }
    esp_mqtt_client_subscribe(mqtt_client, "home/gateway/cmd", 0);
    esp_mqtt_client_subscribe(mqtt_client, "home/gateway/1527map", 0);
    esp_mqtt_client_subscribe(mqtt_client, "ota/upgrade/command", 0);
    ESP_LOGI(TAG, "Subscribed to all topics");
}

// ==================== 命令处理 ====================

void handle_gateway_command(const char *payload, int len)
{
    if (len >= 6 && strncmp(payload, "UPDATE", 6) == 0) {
        start_ota_update(DEFAULT_FW_URL);
    } else if (len >= 7 && strncmp(payload, "RESTART", 7) == 0) {
        esp_restart();
    } else if (len >= 4 && strncmp(payload, "INFO", 4) == 0) {
        publish_device_info();
    } else if (len >= 5 && strncmp(payload, "QUERY", 5) == 0) {
        query_request = true;
    } else if (len >= 5 && strncmp(payload, "ALLON", 5) == 0) {
        stc15_send_frame(ADDR_BROADCAST, CMD_ON);
        for (int i = 0; i < ROOM_COUNT; i++) {
            room_states[i] = true;
            state_received[i] = true;
            publish_room_state(i);
        }
        verify_after_all = true;
        verify_after_all_time = esp_timer_get_time();
    } else if (len >= 6 && strncmp(payload, "ALLOFF", 6) == 0) {
        stc15_send_frame(ADDR_BROADCAST, CMD_OFF);
        for (int i = 0; i < ROOM_COUNT; i++) {
            room_states[i] = false;
            state_received[i] = true;
            publish_room_state(i);
        }
        verify_after_all = true;
        verify_after_all_time = esp_timer_get_time();
    }
}

// ==================== 1527映射表 ====================

void stc15_send_map_set(const uint8_t *entries, int count)
{
    int frame_len = 5 + count * MAP_ENTRY_SIZE + 2;
    uint8_t *frame = (uint8_t *)malloc(frame_len);
    if (!frame) {
        ESP_LOGE(TAG, "Map table frame malloc failed");
        return;
    }
    int pos = 0;
    frame[pos++] = FRAME_HEAD1;
    frame[pos++] = FRAME_HEAD2;
    frame[pos++] = ADDR_BROADCAST;
    frame[pos++] = CMD_SET_MAP;
    frame[pos++] = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        frame[pos++] = entries[i * MAP_ENTRY_SIZE + 0];
        frame[pos++] = entries[i * MAP_ENTRY_SIZE + 1];
        frame[pos++] = entries[i * MAP_ENTRY_SIZE + 2];
        frame[pos++] = entries[i * MAP_ENTRY_SIZE + 3];
    }
    frame[pos++] = FRAME_TAIL1;
    frame[pos++] = FRAME_TAIL2;

    uart_write_bytes(STC15_UART_NUM, frame, pos);
    free(frame);

    ESP_LOGI(TAG, "STC15W <-- Map table sent (%d entries)", count);
}

void publish_1527map_resp(const uint8_t *entries, int count)
{
    if (!mqtt_connected || !mqtt_client) return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "map_data");
    cJSON *data = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        char addr_str[8], key_str[6], dev_str[6];
        snprintf(addr_str, sizeof(addr_str), "%02X%02X",
                 entries[i * MAP_ENTRY_SIZE + 0], entries[i * MAP_ENTRY_SIZE + 1]);
        snprintf(key_str, sizeof(key_str), "%02X", entries[i * MAP_ENTRY_SIZE + 2]);
        snprintf(dev_str, sizeof(dev_str), "%02X", entries[i * MAP_ENTRY_SIZE + 3]);
        cJSON_AddStringToObject(entry, "rf_addr", addr_str);
        cJSON_AddStringToObject(entry, "rf_key", key_str);
        cJSON_AddStringToObject(entry, "dev_addr", dev_str);
        cJSON_AddItemToArray(data, entry);
    }
    cJSON_AddItemToObject(root, "data", data);

    char *json = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(mqtt_client, "home/gateway/1527map_resp", json, 0, 1, 0);
    cJSON_free(json);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Map table published to MQTT (%d entries)", count);
}

void handle_1527map_command(const char *payload, int len)
{
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) {
        ESP_LOGW(TAG, "1527map JSON parse failed");
        return;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
    if (!cmd_item || !cJSON_IsString(cmd_item)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd_item->valuestring, "set_map") == 0) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (!data || !cJSON_IsArray(data)) {
            cJSON_Delete(root);
            return;
        }
        int count = cJSON_GetArraySize(data);
        if (count <= 0 || count > MAP_MAX_ENTRIES) {
            cJSON_Delete(root);
            return;
        }

        uint8_t *entries = (uint8_t *)calloc(count, MAP_ENTRY_SIZE);
        if (!entries) {
            cJSON_Delete(root);
            return;
        }

        bool parse_ok = true;
        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(data, i);
            if (!item) { parse_ok = false; break; }

            cJSON *addr_item = cJSON_GetObjectItem(item, "rf_addr");
            cJSON *key_item = cJSON_GetObjectItem(item, "rf_key");
            cJSON *dev_item = cJSON_GetObjectItem(item, "dev_addr");

            if (!addr_item || !key_item || !dev_item) { parse_ok = false; break; }

            unsigned int addr_val, key_val, dev_val;
            if (sscanf(addr_item->valuestring, "%04X", &addr_val) != 1 ||
                sscanf(key_item->valuestring, "%02X", &key_val) != 1 ||
                sscanf(dev_item->valuestring, "%02X", &dev_val) != 1) {
                parse_ok = false;
                break;
            }

            entries[i * MAP_ENTRY_SIZE + 0] = (addr_val >> 8) & 0xFF;
            entries[i * MAP_ENTRY_SIZE + 1] = addr_val & 0xFF;
            entries[i * MAP_ENTRY_SIZE + 2] = key_val & 0xFF;
            entries[i * MAP_ENTRY_SIZE + 3] = dev_val & 0xFF;
        }

        if (parse_ok) {
            stc15_send_map_set(entries, count);
        }
        free(entries);

    } else if (strcmp(cmd_item->valuestring, "get_map") == 0) {
        stc15_send_frame(ADDR_BROADCAST, CMD_GET_MAP);
        // TODO: Set map_echo_pending flag
        ESP_LOGI(TAG, "Requested STC15 to echo map table");
    }

    cJSON_Delete(root);
}

// ==================== OTA远程升级 ====================

static void ota_task(void *pvParameter)
{
    char *url = (char *)pvParameter;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Starting OTA update...");
    ESP_LOGI(TAG, "Current version: %s", CURRENT_VERSION);
    ESP_LOGI(TAG, "Target firmware: %s", url);
    ESP_LOGI(TAG, "========================================");

    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 60000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };
    esp_task_wdt_reconfigure(&wdt_config);

    esp_http_client_config_t http_config = {
        .url = url,
        .cert_pem = NULL,
        .timeout_ms = 30000,
        .skip_cert_common_name_check = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);

    wdt_config.timeout_ms = WDT_TIMEOUT_S * 1000;
    esp_task_wdt_reconfigure(&wdt_config);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA success, restarting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
        ESP_LOGI(TAG, "Continue running current version");
    }

    free(url);
    vTaskDelete(NULL);
}

void start_ota_update(const char *url)
{
    char *url_copy = strdup(url);
    if (url_copy == NULL) {
        ESP_LOGE(TAG, "OTA URL malloc failed");
        return;
    }
    if (xTaskCreate(ota_task, "ota_task", 8192, url_copy, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "OTA task create failed");
        free(url_copy);
    }
}

void handle_ota_command(const char *payload, int len)
{
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) {
        ESP_LOGW(TAG, "OTA JSON parse failed, using default URL");
        start_ota_update(DEFAULT_FW_URL);
        return;
    }

    cJSON *target_item = cJSON_GetObjectItem(root, "target");
    cJSON *version_item = cJSON_GetObjectItem(root, "version");
    cJSON *fw_url_item = cJSON_GetObjectItem(root, "firmware_url");

    const char *target = (target_item && cJSON_IsString(target_item)) ? target_item->valuestring : "all";
    const char *version = (version_item && cJSON_IsString(version_item)) ? version_item->valuestring : "latest";

    if (strcmp(target, "all") != 0 && strcmp(target, DEVICE_ID) != 0) {
        cJSON_Delete(root);
        return;
    }

    char url[256] = {0};
    if (fw_url_item && cJSON_IsString(fw_url_item) && strlen(fw_url_item->valuestring) > 0) {
        strlcpy(url, fw_url_item->valuestring, sizeof(url));
    } else {
        if (strcmp(version, "latest") == 0) {
            strlcpy(url, DEFAULT_FW_URL, sizeof(url));
        } else {
            if (strcmp(version, CURRENT_VERSION) == 0) {
                ESP_LOGI(TAG, "Target version same as current, skip upgrade");
                cJSON_Delete(root);
                return;
            }
            snprintf(url, sizeof(url), FW_BASE_URL "/%s/firmware.bin", version);
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "OTA upgrade: %s -> %s", CURRENT_VERSION, version);
    start_ota_update(url);
}
