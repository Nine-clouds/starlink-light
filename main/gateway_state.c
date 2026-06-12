/**
 * 网关全局状态模块
 *
 * 集中管理所有跨模块共享的状态变量:
 *   - 房间配置（ID、名称、实体ID）
 *   - MQTT/WiFi连接状态
 *   - 配网状态（AP模式、超时）
 *   - 查询队列状态（逐房间查询、延迟验证）
 *   - 状态发布节流
 */

#include "gateway_state.h"
#include "mqtt_client.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// ==================== 房间配置 ====================
// 6个房间: 晨曦/微风/星光/月影/云栖/霁月
const uint8_t ROOM_IDS[ROOM_COUNT] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
const char *ROOM_NAMES[ROOM_COUNT] = {"晨曦", "微风", "星光", "月影", "云栖", "霁月"};
const char *ROOM_ENTITY_IDS[ROOM_COUNT] = {"sl_cx", "sl_wf", "sl_xg", "sl_yy", "sl_yq", "sl_jy"};

// ==================== 全局状态 ====================
bool room_states[ROOM_COUNT] = {false};
bool state_received[ROOM_COUNT] = {false};
esp_mqtt_client_handle_t mqtt_client = NULL;
bool mqtt_connected = false;
bool wifi_connected = false;
bool provisioning_active = false;
volatile bool need_provisioning = false;
volatile bool provisioning_timeout = false;
esp_timer_handle_t provisioning_timer = NULL;
bool ap_attempted = false;
volatile bool in_mqtt_context = false;
httpd_handle_t config_server = NULL;

EventGroupHandle_t s_wifi_event_group;

int s_retry_num = 0;
int64_t last_heartbeat_us = 0;
int64_t mqtt_connect_time_us = 0;

// ==================== 查询队列状态 ====================
// 两轮查询: 第1轮遍历所有房间，第2轮仅查未回复的房间
int query_index = 0;
int64_t query_last_us = 0;
int query_pass = 0;
volatile bool query_pending = false;
volatile bool query_request = false;
bool verify_after_all = false;
int64_t verify_after_all_time = 0;

// ==================== 状态发布节流 ====================
// 同一房间300ms内不重复发布状态
int64_t last_state_publish_ms[ROOM_COUNT] = {0};

void gateway_state_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
}
