/**
 * Gateway State - Shared state variables
 */

#include "gateway_state.h"
#include "mqtt_client.h"
#include "esp_http_server.h"

// ==================== Room Configuration ====================
const uint8_t ROOM_IDS[ROOM_COUNT] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
const char *ROOM_NAMES[ROOM_COUNT] = {"晨曦", "微风", "星光", "月影", "云栖", "霁月"};
const char *ROOM_ENTITY_IDS[ROOM_COUNT] = {"sl_cx", "sl_wf", "sl_xg", "sl_yy", "sl_yq", "sl_jy"};

// ==================== Global State ====================
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

// ==================== Query Queue State ====================
int query_index = 0;
int64_t query_last_us = 0;
int query_pass = 0;
volatile bool query_pending = false;
volatile bool query_request = false;
bool verify_after_all = false;
int64_t verify_after_all_time = 0;

// ==================== State Publish Throttling ====================
int64_t last_state_publish_ms[ROOM_COUNT] = {0};

void gateway_state_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
}
