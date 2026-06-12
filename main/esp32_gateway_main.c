/**
 * ESP32 智能灯控网关 (ESP-IDF)
 *
 * 架构: Home Assistant ──MQTT──▶ESP32 ──UART──▶STC15W ──HC-12/RF──▶灯控
 *
 * ESP32↔STC15W 串口协议 (9600 baud, UART1 GPIO7/GPIO6):
 *   命令帧: 10 18 [地址] [命令] 18 10         (6字节)
 *   回复帧: 10 18 [地址] [命令] [状态] 18 10  (7字节)
 *
 *   地址: 0x01~0x06=房间, 0xFF=广播
 *   命令: 0x01=开灯, 0x02=关灯, 0x03=查询, 0x04=翻转, 0xA0=写映射表, 0xA1=读映射表
 *   状态: 1=开, 0=关 (仅回复帧)
 *
 * WiFi配网: 有凭据优先连WiFi, 5次失败弹AP(60s), 一次上电只弹一次
 *           无凭据直接开AP等待配网
 *           访问 http://192.168.4.1 配置WiFi信息
 *
 * 远程日志: WARN/ERROR 自动上报 MQTT -> home/gateway/log
 *
 * 服务器地址等配置见 main/config.h (将 config.example.h 重命名为 config.h 后填写)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mqtt_client.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "config.h"

static const char *TAG = "gateway";

// ==================== MQTT 配置 ====================
#define MQTT_LOG_TOPIC     "home/gateway/log"
#define CURRENT_VERSION    "v1.0.7"

// ==================== 房间配置 ====================
#define ROOM_COUNT         6
static const uint8_t ROOM_IDS[ROOM_COUNT] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
static const char *ROOM_NAMES[ROOM_COUNT] = {"晨曦", "微风", "星光", "月影", "云栖", "霁月"};
static const char *ROOM_ENTITY_IDS[ROOM_COUNT] = {"sl_cx", "sl_wf", "sl_xg", "sl_yy", "sl_yq", "sl_jy"};

// ==================== STC15W 帧协议 ====================
#define FRAME_HEAD1        0x10
#define FRAME_HEAD2        0x18
#define FRAME_TAIL1        0x18
#define FRAME_TAIL2        0x10
#define ADDR_BROADCAST     0xFF
#define CMD_ON             0x01
#define CMD_OFF            0x02
#define CMD_QUERY          0x03
#define CMD_TOGGLE         0x04
#define CMD_SET_MAP        0xA0    // 写入1527映射
#define CMD_GET_MAP        0xA1    // 读取1527映射
#define MAP_ENTRY_SIZE     4       // 每条映射: addr_h + addr_l + key + dev_addr
#define MAP_MAX_ENTRIES    32      // 最大映射条
// ==================== UART 配置 ====================
#define STC15_UART_NUM     UART_NUM_1
#define STC15_UART_TX_PIN  7
#define STC15_UART_RX_PIN  6
#define STC15_UART_BAUD    9600
#define STC15_UART_BUF     256

// ==================== WiFi 配网 ====================
#define AP_SSID            "Cloud_Hub"
#define AP_PASSWORD        "12345678"
#define AP_MAX_CONN        4

// ==================== 定时参数 ====================
#define HEARTBEAT_INTERVAL_MS   60000
#define RX_TIMEOUT_US           50000   // 50ms
#define TX_INTERVAL_US          80000   // 80ms
#define MAX_TX_WAIT_US          50000   // 发送间隔最大等待50ms, 超限直接发
#define STATE_PUBLISH_MIN_MS    300     // 状态上报间隔300ms
#define VERIFY_DELAY_US         1500000 // 广播命令后1.5秒验证
#define WIFI_MAX_RETRY          5
#define WIFI_CONNECT_TIMEOUT_MS 30000
#define PROVISIONING_TIMEOUT_S  60      // 上电后AP配网窗口60
#define WDT_TIMEOUT_S           10      // 看门狗超时10
// ==================== 全局状态 ====================
static bool room_states[ROOM_COUNT] = {false};
static bool state_received[ROOM_COUNT] = {false};
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;
static bool wifi_connected = false;
static bool provisioning_active = false;
static volatile bool need_provisioning = false;  // 由事件处理器设置，主循环中执行
static volatile bool provisioning_timeout = false; // AP配网窗口超时，切换到STA
static esp_timer_handle_t provisioning_timer = NULL;
static bool ap_attempted = false;       // 已尝试过一次AP配网，不再重复
static volatile bool in_mqtt_context = false;  // 防止日志上报时MqTT重入
static httpd_handle_t config_server = NULL;

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static int s_retry_num = 0;
static int64_t last_heartbeat_us = 0;
static int64_t mqtt_connect_time_us = 0;  // MQTT连接时间

// STC15W 接收状态机
typedef enum {
    RX_IDLE, RX_HEAD1, RX_HEAD2, RX_ADDR, RX_CMD, RX_DATA, RX_TAIL1
} rx_state_t;
static rx_state_t rx_state = RX_IDLE;
static uint8_t rx_addr, rx_cmd, rx_data;
static bool rx_has_data = false;
static int64_t rx_frame_start_us = 0;

// STC15W 发送节流
static int64_t last_tx_time_us = 0;

// 查询队列: 逐房间发送QUERY, 间隔200ms避免HC-12半双工冲突
static int query_index = 0;
static int64_t query_last_us = 0;
static int query_pass = 0;  // 0=未开始, 1=首轮查询, 2=补查
static volatile bool query_pending = false;  // 有查询任务待执行
static volatile bool query_request = false;  // MQTT事件请求启动查询
static bool verify_after_all = false;        // 广播命令后需延迟验证
static int64_t verify_after_all_time = 0;    // 延迟验证起始时间
#define QUERY_INTERVAL_US  200000  // 200ms

// 状态上报节流
static int64_t last_state_publish_ms[ROOM_COUNT] = {0};

// 1527映射表接收缓冲 (STC15回传映射表时的状态机)
typedef enum {
    MAP_RX_IDLE, MAP_RX_COUNT, MAP_RX_DATA, MAP_RX_TAIL1, MAP_RX_TAIL2
} map_rx_state_t;
static map_rx_state_t map_rx_state = MAP_RX_IDLE;
static uint8_t map_rx_count = 0;
static uint8_t map_rx_buf[MAP_MAX_ENTRIES * MAP_ENTRY_SIZE];
static uint8_t map_rx_idx = 0;
static int64_t map_rx_start_us = 0;    // 映射表接收起始时间
static bool map_echo_pending = false;  // 正在等待STC15回传映射
static int64_t map_echo_sent_us = 0;   // 映射表命令发送时间
// ==================== 函数声明 ====================
static void wifi_init(void);
static void provisioning_timer_cb(void *arg);
static void start_provisioning(void);
static void mqtt_app_start(void);
static void flush_log_buffer(void);
static void stc15_uart_init(void);
static void stc15_send_frame(uint8_t addr, uint8_t cmd);
static void stc15_process_rx(void);
static void publish_room_state(int index);
static void publish_device_info(void);
static void send_auto_discovery_all(void);
static void subscribe_all(void);
static void start_ota_update(const char *url);
static void handle_mqtt_message(esp_mqtt_event_handle_t event);
static void handle_ota_command(const char *payload, int len);
static void handle_gateway_command(const char *payload, int len);
static void handle_1527map_command(const char *payload, int len);
static void stc15_send_map_set(const uint8_t *data, int count);
static void publish_1527map_resp(const uint8_t *data, int count);
static void dns_server_start(void);
static void dns_server_stop(void);

// ==================== DNS 劫持服务 (Captive Portal) ====================
static int dns_sock = -1;
static volatile bool dns_running = false;
static TaskHandle_t dns_task_handle = NULL;

static void dns_task(void *pvParameters)
{
    dns_running = true;

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(53);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    dns_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (dns_sock < 0) {
        ESP_LOGE(TAG, "DNS: 创建socket失败");
        dns_running = false;
        vTaskDelete(NULL);
        return;
    }

    // 允许地址复用，防止端口被占用
    int optval = 1;
    setsockopt(dns_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    // 设置接收超时，以便可以检查 dns_running 标志
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (bind(dns_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS: bind失败 (errno=%d)", errno);
        close(dns_sock);
        dns_sock = -1;
        dns_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS劫持服务器已启动 (所有域名 -> 192.168.4.1)");

    // AP 的 IP 地址 192.168.4.1
    uint8_t ap_ip[4] = {192, 168, 4, 1};

    while (dns_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        struct iovec iov;
        struct msghdr msg;
        uint8_t rx_buf[128];

        memset(&msg, 0, sizeof(msg));
        msg.msg_name = &client_addr;
        msg.msg_namelen = client_len;
        iov.iov_base = rx_buf;
        iov.iov_len = sizeof(rx_buf);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        int recv_len = recvmsg(dns_sock, &msg, 0);
        if (recv_len < 0) {
            // 超时或错误，检查 dns_running 后继
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (!dns_running) break;
            continue;
        }
        if (recv_len < 12 || recv_len > 240) continue;  // DNS 最小12字节头，最大240防止溢出

        // 构建 DNS 响应：复用请求头 + 问题 + 应答
uint8_t tx_buf[256];
        int tx_len = recv_len;

        memcpy(tx_buf, rx_buf, recv_len);

        // 设置响应标志
        tx_buf[2] = 0x81;  // QR=1, OPCODE=0, AA=0, TC=0, RD=1
        tx_buf[3] = 0x80;  // RA=1, Z=0, RCODE=0
        tx_buf[6] = 0x00; tx_buf[7] = 0x01;  // ANCOUNT = 1

        // 追加应答段：指针到问题段 + Type A + Class IN + TTL + IP
        int pos = recv_len;
        tx_buf[pos++] = 0xC0; tx_buf[pos++] = 0x0C;  // 指针偏移12 (问题
        tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x01;  // Type A
        tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x01;  // Class IN
        tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x00;  // TTL 4字节
        tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x3C;  // TTL = 60s
        tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x04;  // 数据长度 4
        tx_buf[pos++] = ap_ip[0];
        tx_buf[pos++] = ap_ip[1];
        tx_buf[pos++] = ap_ip[2];
        tx_buf[pos++] = ap_ip[3];

        tx_len = pos;

        sendto(dns_sock, tx_buf, tx_len, 0,
               (struct sockaddr *)&client_addr, sizeof(client_addr));
    }

    if (dns_sock >= 0) {
        close(dns_sock);
        dns_sock = -1;
    }
    dns_running = false;
    vTaskDelete(NULL);
}

static void dns_server_start(void)
{
    if (dns_running) return;
    xTaskCreate(dns_task, "dns_server", 4096, NULL, 2, &dns_task_handle);
}

static void dns_server_stop(void)
{
    dns_running = false;
    if (dns_sock >= 0) {
        shutdown(dns_sock, SHUT_RDWR);
    }
    // 等待DNS任务真正退出(最多1秒)
    if (dns_task_handle) {
        for (int i = 0; i < 10 && eTaskGetState(dns_task_handle) != eDeleted; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        dns_task_handle = NULL;
    }
}

// ==================== Captive Portal 重定向====================

// 拦截所有未匹配 URL (404), 302 重定向到配网
static esp_err_t captive_portal_handler(httpd_req_t *req, httpd_err_code_t err)
{
    // 302 重定向到配网首页
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}
#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "password"

static esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK && password) {
        err = nvs_set_str(handle, NVS_KEY_PASS, password);
    }
    nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t load_wifi_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, NVS_KEY_PASS, password, &pass_len);
    }
    nvs_close(handle);
    return err;
}

// ==================== WiFi 配网 HTTP 服务====================

static const char CONFIG_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Cloud Hub</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
    "min-height:100vh;background:linear-gradient(135deg,#0f0c29,#302b63,#24243e);"
    "display:flex;align-items:center;justify-content:center;padding:20px}"
    ".card{width:100%;max-width:380px;background:rgba(255,255,255,.07);"
    "backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px);"
    "border-radius:20px;border:1px solid rgba(255,255,255,.12);padding:32px 24px;"
    "box-shadow:0 8px 32px rgba(0,0,0,.3);color:#fff}"
    ".logo{text-align:center;margin-bottom:24px}"
    ".logo .icon{width:56px;height:56px;border-radius:16px;"
    "background:linear-gradient(135deg,#667eea,#764ba2);display:inline-flex;"
    "align-items:center;justify-content:center;font-size:28px;margin-bottom:12px}"
    ".logo h1{font-size:22px;font-weight:600;letter-spacing:.5px}"
    ".logo p{font-size:13px;color:rgba(255,255,255,.5);margin-top:4px}"
    ".field{margin-bottom:16px;position:relative}"
    ".field label{display:block;font-size:12px;font-weight:500;"
    "color:rgba(255,255,255,.5);margin-bottom:6px;letter-spacing:.3px}"
    ".field input{width:100%;padding:12px 14px;background:rgba(255,255,255,.08);"
    "border:1px solid rgba(255,255,255,.1);border-radius:10px;color:#fff;"
    "font-size:15px;outline:none;transition:border .2s}"
    ".field input::placeholder{color:rgba(255,255,255,.25)}"
    ".field input:focus{border-color:rgba(102,126,234,.6)}"
    ".scan-btn{width:100%;padding:12px;border:1px dashed rgba(255,255,255,.2);"
    "border-radius:10px;background:transparent;color:rgba(255,255,255,.6);"
    "font-size:14px;cursor:pointer;transition:all .2s;margin-bottom:16px;"
    "display:flex;align-items:center;justify-content:center;gap:6px}"
    ".scan-btn:hover{background:rgba(255,255,255,.06);border-color:rgba(255,255,255,.3)}"
    ".scan-btn:disabled{opacity:.5;cursor:not-allowed}"
    "#wifiList{list-style:none;max-height:220px;overflow-y:auto;margin-bottom:16px;"
    "border-radius:10px;border:1px solid rgba(255,255,255,.08);display:none}"
    "#wifiList::-webkit-scrollbar{width:4px}"
    "#wifiList::-webkit-scrollbar-thumb{background:rgba(255,255,255,.15);border-radius:2px}"
    "#wifiList li{padding:11px 14px;border-bottom:1px solid rgba(255,255,255,.06);"
    "cursor:pointer;display:flex;align-items:center;justify-content:space-between;"
    "transition:background .15s}"
    "#wifiList li:last-child{border-bottom:none}"
    "#wifiList li:hover{background:rgba(255,255,255,.08)}"
    "#wifiList li:active{background:rgba(102,126,234,.2)}"
    "#wifiList .ssid{font-size:14px;font-weight:500}"
    "#wifiList .meta{display:flex;align-items:center;gap:8px}"
    "#wifiList .bars{display:inline-flex;gap:1px;align-items:flex-end;height:12px}"
    "#wifiList .bars span{width:3px;background:rgba(255,255,255,.3);border-radius:1px}"
    "#wifiList .rssi{font-size:11px;color:rgba(255,255,255,.4)}"
    ".loading{text-align:center;padding:14px;color:rgba(255,255,255,.35);font-size:13px}"
    ".submit-btn{width:100%;padding:13px;border:none;border-radius:10px;"
    "background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;"
    "font-size:15px;font-weight:600;cursor:pointer;transition:all .2s;"
    "letter-spacing:.3px;margin-top:4px}"
    ".submit-btn:hover{transform:translateY(-1px);box-shadow:0 4px 15px rgba(102,126,234,.4)}"
    ".submit-btn:active{transform:translateY(0)}"
    "#msg{text-align:center;margin-top:14px;font-size:13px;"
    "color:rgba(255,255,255,.6);min-height:20px;transition:color .2s}"
    "#msg.error{color:#ff6b6b}"
    "#msg.success{color:#51cf66}"
    ".pw-toggle{position:absolute;right:12px;top:32px;background:none;border:none;"
    "color:rgba(255,255,255,.35);cursor:pointer;font-size:18px;padding:4px}"
    ".pw-toggle:hover{color:rgba(255,255,255,.6)}"
    "</style></head><body>"
    "<div class='card'>"
    "<div class='logo'>"
    "<div class='icon'>&#9889;</div>"
    "<h1>Cloud Hub</h1>"
    "<p>WiFi 配置</p>"
    "</div>"
    "<button class='scan-btn' id='scanBtn' onclick='scanWifi()'>&#128225; 扫描附近WiFi</button>"
    "<ul id='wifiList'></ul>"
    "<div class='field'>"
    "<label>WiFi 名称</label>"
    "<input id='ssid' placeholder='点击上方列表或手动输入' required>"
    "</div>"
    "<div class='field'>"
    "<label>WiFi 密码</label>"
    "<input id='password' type='password' placeholder='输入密码'>"
    "<button class='pw-toggle' id='pwBtn' onclick='togglePw()'>"
    "<svg id='eyeOpen' width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' style='display:none'>"
    "<path d='M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z'/><circle cx='12' cy='12' r='3'/>"
    "</svg>"
    "<svg id='eyeClosed' width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'>"
    "<path d='M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24'/>"
    "<line x1='1' y1='1' x2='23' y2='23'/>"
    "</svg>"
    "</button>"
    "</div>"
    "<button class='submit-btn' onclick='save()'>保存并连接/button>"
    "<p id='msg'></p>"
    "</div>"
    "<script>"
    "function rssiBars(r){"
    "var n=r>=-50?4:r>=-60?3:r>=-70?2:1;"
    "var h=[3,6,9,12],s='';"
    "for(var i=0;i<4;i++)s+='<span style=\"height:'+h[i]+'px;opacity:'+(i<n?.7:.2)+'\"></span>';"
    "return s}"
    "function scanWifi(){"
    "var btn=document.getElementById('scanBtn');"
    "var list=document.getElementById('wifiList');"
    "btn.disabled=true;btn.textContent='扫描中..';"
    "list.style.display='block';"
    "list.innerHTML='<li class=\"loading\">正在扫描附近网络...</li>';"
    "fetch('/scan').then(r=>r.json()).then(function(data){"
    "btn.disabled=false;btn.innerHTML='&#128225; 重新扫描';"
    "if(!data||!data.length){list.innerHTML='<li class=\"loading\">未找到WiFi网络</li>';return;}"
    "data.sort(function(a,b){return b.rssi-a.rssi;});"
    "list.innerHTML=data.map(function(ap){"
    "var lock=ap.auth?'&#128274;':'';"
    "return '<li onclick=\"pickSsid(\\''+ap.ssid.replace(/'/g,\"\\\\'\")+'\\')\">'+"
    "'<span class=\"ssid\">'+lock+' '+ap.ssid+'</span>'+"
    "'<span class=\"meta\"><span class=\"bars\">'+rssiBars(ap.rssi)+'</span>'+"
    "'<span class=\"rssi\">'+ap.rssi+'</span></span></li>';"
    "}).join('');"
    "}).catch(function(e){"
    "btn.disabled=false;btn.innerHTML='&#128225; 重新扫描';"
    "list.innerHTML='<li class=\"loading\">扫描失败，请重试</li>';"
    "});}"
    "function pickSsid(ssid){"
    "document.getElementById('ssid').value=ssid;"
    "document.getElementById('password').focus();}"
    "function togglePw(){"
    "var inp=document.getElementById('password');"
    "var open=document.getElementById('eyeOpen');"
    "var closed=document.getElementById('eyeClosed');"
"if(inp.type==='password'){inp.type='text';open.style.display='inline';closed.style.display='none';}"
"else{inp.type='password';open.style.display='none';closed.style.display='inline';}}"
    "function save(){"
    "var msg=document.getElementById('msg');"
    "msg.className='';"
    "var s=document.getElementById('ssid').value;"
    "var p=document.getElementById('password').value;"
    "if(!s){msg.textContent='请输入WiFi名称';msg.className='error';return;}"
    "msg.textContent='正在连接...';"
    "fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({ssid:s,password:p})})"
    ".then(r=>r.text()).then(t=>{msg.textContent=t;msg.className='success';})"
    ".catch(e=>{msg.textContent='保存失败';msg.className='error';});}"
    "</script></body></html>";

static esp_err_t config_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CONFIG_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_OK;  // 已发送响应，返回ESP_OK
    }
    char *buf = malloc(content_len + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    int total = 0;
    int retry = 0;
    while (total < content_len) {
        int ret = httpd_req_recv(req, buf + total, content_len - total);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++retry > 10) {
                    free(buf);
                    httpd_resp_send_408(req);
                    return ESP_OK;
                }
                continue;
            }
            free(buf);
            return ESP_FAIL;  // 连接异常，未发送响应
        }
        total += ret;
        retry = 0;
    }
    buf[total] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        free(buf);
        return ESP_OK;
    }

    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");

    if (!ssid_item || !cJSON_IsString(ssid_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        free(buf);
        return ESP_OK;
    }

    const char *ssid = ssid_item->valuestring;
    const char *password = (pass_item && cJSON_IsString(pass_item)) ? pass_item->valuestring : "";

    ESP_LOGI(TAG, "收到WiFi配置: SSID=%s", ssid);

    esp_err_t err = save_wifi_credentials(ssid, password);
    cJSON_Delete(root);
    free(buf);

    if (err == ESP_OK) {
        httpd_resp_send(req, "保存成功，设备将重启...", HTTPD_RESP_USE_STRLEN);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ESP_OK;
    }
    return ESP_OK;
}

static esp_err_t config_scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 60, .max = 100 },
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi扫描失败: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "[]", 2);
        return ESP_OK;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "[]", 2);
        return ESP_OK;
    }

    if (ap_count > 20) ap_count = 20;

    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_list) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "[]", 2);
        return ESP_OK;
    }

    if (esp_wifi_scan_get_ap_records(&ap_count, ap_list) != ESP_OK) {
        free(ap_list);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "[]", 2);
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", (const char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", ap_list[i].rssi);
        cJSON_AddBoolToObject(ap, "auth", ap_list[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(root, ap);
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));

    cJSON_free(json);
    cJSON_Delete(root);
    free(ap_list);

    return ESP_OK;
}

static httpd_handle_t start_config_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = 12;  // 配网时手机并发请求较多，提高上限
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;  // 满连接时踢掉最旧的，保证新请求能进
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "启动HTTP服务器失败");
        return NULL;
    }

    // 先注册具体路径（优先匹配）
    httpd_uri_t get_uri = { .uri = "/", .method = HTTP_GET, .handler = config_get_handler };
    httpd_uri_t post_uri = { .uri = "/save", .method = HTTP_POST, .handler = config_post_handler };
    httpd_uri_t scan_uri = { .uri = "/scan", .method = HTTP_GET, .handler = config_scan_handler };

    httpd_register_uri_handler(server, &get_uri);
    httpd_register_uri_handler(server, &post_uri);
    httpd_register_uri_handler(server, &scan_uri);

    // 注册自定义 404 处理器：拦截所有未匹配的请求，302 跳转到配网页
    // 这样 Android/Apple/Windows 等各种连通性检查 URL 都会被捕获
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, captive_portal_handler);

    // 启动 DNS 劫持服务
dns_server_start();

    ESP_LOGI(TAG, "HTTP配置服务+DNS已启动 (Captive Portal)");
    return server;
}

static void stop_config_server(void)
{
    dns_server_stop();
    if (config_server) {
        httpd_stop(config_server);
        config_server = NULL;
    }
}

// ==================== WiFi 事件处理 ====================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            if (!provisioning_active) {
                ESP_LOGI(TAG, "WiFi STA 启动，尝试连接..");
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            wifi_connected = false;
            if (provisioning_active) break;  // 已在配网模式，忽略断连事件
            s_retry_num++;
            if (s_retry_num <= WIFI_MAX_RETRY) {
                ESP_LOGI(TAG, "WiFi重连接.. (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
                esp_wifi_connect();
            } else if (!ap_attempted) {
                ESP_LOGW(TAG, "WiFi连接失败%d次，启动配网模式(%ds)", WIFI_MAX_RETRY, PROVISIONING_TIMEOUT_S);
                ap_attempted = true;
                need_provisioning = true;
            } else {
                ESP_LOGW(TAG, "WiFi连接失败，继续重试..");
                s_retry_num = 0;
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_CONNECTED:
            s_retry_num = 0;
            ESP_LOGI(TAG, "WiFi已关联到AP");
            break;
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "配网AP已启动: %s", AP_SSID);
            // 手动设置 DHCP 服务器的 DNS 选项，让客户端使用 192.168.4.1 作为 DNS
            // （CONFIG_LWIP_DHCPS_ADD_DNS=n 阻止了内置DNS，需要手动通告）
            {
                esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
                if (ap_netif) {
                    uint32_t dns_ip = ipaddr_addr("192.168.4.1");
                    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                        ESP_NETIF_DOMAIN_NAME_SERVER, &dns_ip, sizeof(dns_ip));
                    ESP_LOGI(TAG, "DHCP DNS选项已设置: 192.168.4.1");
                }
            }
            if (!config_server) {
                config_server = start_config_server();
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "有设备连接到配网AP");
            break;
        case WIFI_EVENT_AP_STOP:
            // AP 停止时清理服务器（正常流程中 start_provisioning 已提前调用）
            if (config_server) {
                stop_config_server();
            }
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "获取IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
        provisioning_active = false;
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (!mqtt_client) {
            mqtt_app_start();
        }
    }
}

// 配网超时回调（esp_timer回调中不能操作WiFi，只设标志）
static void provisioning_timer_cb(void *arg)
{
    provisioning_timeout = true;
}

static void start_provisioning(void)
{
    if (provisioning_active) return;  // 已在配网模式，防止重复进入
    provisioning_active = true;
    s_retry_num = 0;

    // 停止 MQTT 客户端，防止其不断重连占用 STA 接口导致扫描失败
    if (mqtt_client) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        mqtt_connected = false;
        ESP_LOGI(TAG, "MQTT客户端已停止");
    }

    // 必须先停掉 HTTP/DNS 服务器！
    // esp_wifi_deinit() 会销毁网络接口和所有 socket
    // 如果不先停服务器，httpd 的监听 socket 会被销毁但 config_server 仍非 NULL
    // 导致 AP 重启后不会重建服务器
    stop_config_server();

    // 彻底去初始化WiFi驱动，确保内部状态完全清除
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    vTaskDelay(pdMS_TO_TICKS(500));

    // 重新创建WiFi网络接口（esp_wifi_deinit会销毁netif）
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    // 重新初始化WiFi驱动（全新状态）
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, AP_SSID, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(AP_SSID);
    ap_config.ap.max_connection = AP_MAX_CONN;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    strlcpy((char *)ap_config.ap.password, AP_PASSWORD, sizeof(ap_config.ap.password));

    // APSTA 模式：AP 供手机连接，STA 用于扫描 WiFi
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
    // 清空 STA 配置，确保不会自动连接
    wifi_config_t sta_blank = {0};
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_blank));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "请连接WiFi '%s'（密码 %s）并访问 http://192.168.4.1 进行配网", AP_SSID, AP_PASSWORD);
}

// AP配网60秒超时, 关闭AP，切换为STA模式重连WiFi
static void switch_to_sta_mode(void)
{
    // 清理定时
if (provisioning_timer) {
        esp_timer_stop(provisioning_timer);
        esp_timer_delete(provisioning_timer);
        provisioning_timer = NULL;
    }

    // 读取已保存的凭据
    char ssid[33] = {0}, password[65] = {0};
    esp_err_t err = load_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password));
    if (err != ESP_OK || strlen(ssid) == 0) {
        ESP_LOGW(TAG, "超时但无WiFi凭据，保持AP模式");
        return;
    }

    ESP_LOGI(TAG, "配网窗口结束，切换到STA模式连接: %s", ssid);

    // 停止 HTTP/DNS 服务
stop_config_server();

    // 停止 WiFi
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    vTaskDelay(pdMS_TO_TICKS(500));

    // 重新创建STA网络接口
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta_netif, "starlink-gw1");

    // 重新初始化WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 设置STA模式
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    provisioning_active = false;
    provisioning_timeout = false;
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    // 设置主机名，替代默认的"espressif"
    esp_netif_set_hostname(sta_netif, "starlink-gw1");
    esp_netif_set_hostname(ap_netif, "starlink-gw1");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    char ssid[33] = {0}, password[65] = {0};
    esp_err_t err = load_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password));

    bool has_creds = (err == ESP_OK && strlen(ssid) > 0);

    if (has_creds) {
        // 有凭据：直接尝试连接WiFi
        wifi_config_t wifi_config = {0};
        strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "尝试连接WiFi: %s", ssid);
    } else {
        // 无凭据：开AP配网（常驻）
        ESP_LOGI(TAG, "无WiFi凭据，启动配网模式");
        start_provisioning();
    }
}

// ==================== MQTT 客户端====================

static void publish_room_state(int index)
{
    if (!mqtt_connected || !mqtt_client) return;
    // 节流: 300ms 内不重复上报
    int64_t now = esp_timer_get_time() / 1000;
    if (now - last_state_publish_ms[index] < STATE_PUBLISH_MIN_MS) return;
    last_state_publish_ms[index] = now;

    char topic[64];
    snprintf(topic, sizeof(topic), "home/%s/state", ROOM_ENTITY_IDS[index]);
    const char *state = room_states[index] ? "ON" : "OFF";
    esp_mqtt_client_publish(mqtt_client, topic, state, 0, 1, 1);
}

static void publish_device_info(void)
{
    if (!mqtt_connected || !mqtt_client) return;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", DEVICE_ID);
    cJSON_AddStringToObject(root, "version", CURRENT_VERSION);
    cJSON_AddStringToObject(root, "status", "online");
    // 添加 IP 地址
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
    ESP_LOGI(TAG, "已上报设备信息");
}

static void send_auto_discovery_all(void)
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
    ESP_LOGI(TAG, "已发送自动发现配置");
}

static void send_log_sensor_discovery(void)
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

static void subscribe_all(void)
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
    ESP_LOGI(TAG, "已订阅所有主题");
}

static void handle_gateway_command(const char *payload, int len)
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

// ==================== 1527映射表管理====================

// 发送映射表到STC15 (UART协议: 10 18 FF A0 [count] [entry0..n] 18 10)
static void stc15_send_map_set(const uint8_t *entries, int count)
{
    // 10 18 FF A0 [count] [addr_h addr_l key dev_addr]×N 18 10
    int frame_len = 5 + count * MAP_ENTRY_SIZE + 2;  // 帧头 + 数据 + 帧尾 
    uint8_t *frame = (uint8_t *)malloc(frame_len);
    if (!frame) {
        ESP_LOGE(TAG, "映射表帧内存分配失败");
        return;
    }
    int pos = 0;
    frame[pos++] = FRAME_HEAD1;
    frame[pos++] = FRAME_HEAD2;
    frame[pos++] = ADDR_BROADCAST;   // 映射表用广播地址
    frame[pos++] = CMD_SET_MAP;      // 0xA0
    frame[pos++] = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        frame[pos++] = entries[i * MAP_ENTRY_SIZE + 0];  // addr_h
        frame[pos++] = entries[i * MAP_ENTRY_SIZE + 1];  // addr_l
        frame[pos++] = entries[i * MAP_ENTRY_SIZE + 2];  // key
        frame[pos++] = entries[i * MAP_ENTRY_SIZE + 3];  // dev_addr
    }
    frame[pos++] = FRAME_TAIL1;
    frame[pos++] = FRAME_TAIL2;

    uart_write_bytes(STC15_UART_NUM, frame, pos);
    free(frame);

    // SET_MAP不设置map_echo_pending，只有GET_MAP才会回传映射表数
ESP_LOGI(TAG, "STC15W <-- 映射表发送 (%d条)", count);
}

// 发布映射表到MQTT (响应Web端读取请
static void publish_1527map_resp(const uint8_t *entries, int count)
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

    ESP_LOGI(TAG, "已发布映射表到MQTT (%d条)", count);
}

// 处理Web端发来的1527映射表命令
static void handle_1527map_command(const char *payload, int len)
{
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) {
        ESP_LOGW(TAG, "1527map JSON解析失败");
        return;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
    if (!cmd_item || !cJSON_IsString(cmd_item)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd_item->valuestring, "set_map") == 0) {
        // 写入映射表
        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (!data || !cJSON_IsArray(data)) {
            cJSON_Delete(root);
            ESP_LOGW(TAG, "1527map set_map: data字段缺失");
            return;
        }
        int count = cJSON_GetArraySize(data);
        if (count <= 0 || count > MAP_MAX_ENTRIES) {
            cJSON_Delete(root);
            ESP_LOGW(TAG, "1527map: 条数无效 (%d)", count);
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
                ESP_LOGW(TAG, "1527map: 第%d条hex解析失败", i);
                parse_ok = false;
                break;
            }

            entries[i * MAP_ENTRY_SIZE + 0] = (addr_val >> 8) & 0xFF;  // addr_h
            entries[i * MAP_ENTRY_SIZE + 1] = addr_val & 0xFF;         // addr_l
            entries[i * MAP_ENTRY_SIZE + 2] = key_val & 0xFF;          // key
            entries[i * MAP_ENTRY_SIZE + 3] = dev_val & 0xFF;          // dev_addr
        }

        if (parse_ok) {
            stc15_send_map_set(entries, count);
        } else {
            ESP_LOGW(TAG, "1527map set_map: 数据解析出错");
        }
        free(entries);

    } else if (strcmp(cmd_item->valuestring, "get_map") == 0) {
        // 请求STC15回传映射表
        stc15_send_frame(ADDR_BROADCAST, CMD_GET_MAP);
        map_echo_pending = true;
        map_echo_sent_us = esp_timer_get_time();
        ESP_LOGI(TAG, "已请求STC15回传映射表");
    }

    cJSON_Delete(root);
}

static void handle_ota_command(const char *payload, int len)
{
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) {
        ESP_LOGW(TAG, "OTA JSON解析失败，使用默认URL");
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
                ESP_LOGI(TAG, "目标版本与当前版本相同，跳过升级");
                cJSON_Delete(root);
                return;
            }
            snprintf(url, sizeof(url), FW_BASE_URL "/%s/firmware.bin", version);
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "OTA升级: %s -> %s", CURRENT_VERSION, version);
    start_ota_update(url);
}

static void handle_mqtt_message(esp_mqtt_event_handle_t event)
{
    char topic[128] = {0};
    int tlen = event->topic_len < sizeof(topic) - 1 ? event->topic_len : sizeof(topic) - 1;
    memcpy(topic, event->topic, tlen);

    // OTA 升级指令
    if (strcmp(topic, "ota/upgrade/command") == 0) {
        handle_ota_command(event->data, event->data_len);
        return;
    }

    // 网关全局命令
    if (strcmp(topic, "home/gateway/cmd") == 0) {
        handle_gateway_command(event->data, event->data_len);
        return;
    }

    // 1527映射表命
if (strcmp(topic, "home/gateway/1527map") == 0) {
        handle_1527map_command(event->data, event->data_len);
        return;
    }

    // 房间开关指
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

static void mqtt_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    (void)event_base;
    in_mqtt_context = true;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT已连接");
        mqtt_connected = true;
        mqtt_connect_time_us = esp_timer_get_time();
        last_heartbeat_us = esp_timer_get_time();  // 上报过设备信息，心跳重新计时
        // 先清除所有command topic的残留retain消息（空消息+retain=1覆盖旧值）
        // 必须在subscribe之前清除，否则订阅后broker立即下发旧retain消息触发误动
for (int i = 0; i < ROOM_COUNT; i++) {
            char topic[64];
            snprintf(topic, sizeof(topic), "home/%s/command", ROOM_ENTITY_IDS[i]);
            esp_mqtt_client_publish(mqtt_client, topic, "", 0, 1, 1);
        }
        vTaskDelay(pdMS_TO_TICKS(50));  // 等待清除消息生效
        // 再订阅主题
subscribe_all();
        send_auto_discovery_all();
        send_log_sensor_discovery();
        publish_device_info();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT断开连接");
        mqtt_connected = false;
        break;
    case MQTT_EVENT_DATA:
        handle_mqtt_message((esp_mqtt_event_handle_t)event_data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT错误");
        break;
    default:
        break;
    }
    in_mqtt_context = false;
    flush_log_buffer();  // 发送MQTT处理期间缓存的日
}

static void mqtt_app_start(void)
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
        ESP_LOGE(TAG, "MQTT客户端初始化失败");
        return;
    }
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    ESP_LOGI(TAG, "MQTT客户端已启动");
}

// ==================== MQTT 日志上报 ====================
#define LOG_BUF_SIZE        256
#define LOG_RING_COUNT      8       // MQTT上下文内最多缓存 8 条日志
static vprintf_like_t s_original_vprintf = NULL;
static char log_ring[LOG_RING_COUNT][LOG_BUF_SIZE];
static volatile int log_ring_head = 0;  // 写入位置
static volatile int log_ring_count = 0; // 当前缓存条数

// 将缓存日志发到MQTT（在MQTT上下文外调用）
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

static int mqtt_log_vprintf(const char *fmt, va_list args)
{
    // 复制 va_list，因为原始输出会消费 args
    va_list args_copy;
    va_copy(args_copy, args);

    // 先走原始串口输出，确保本地也能看到日志
    int ret = 0;
    if (s_original_vprintf) {
        ret = s_original_vprintf(fmt, args);
    }

    // 仅上报 WARN / ERROR 级别
    char level = fmt[0];
    if (level != 'W' && level != 'E') {
        va_end(args_copy);
        return ret;
    }

    // MQTT 未连接时丢弃
    if (!mqtt_client || !mqtt_connected) {
        va_end(args_copy);
        return ret;
    }

    char *buf;
    if (in_mqtt_context) {
        // MQTT上下文内: 缓存，等空闲时上报
if (log_ring_count >= LOG_RING_COUNT) {
            // 缓存满，丢弃最旧的一条
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

    // 非MQTT上下文: 直接发送
if (!in_mqtt_context) {
        esp_mqtt_client_publish(mqtt_client, MQTT_LOG_TOPIC, buf, 0, 1, 0);
    }
    return ret;
}

// ==================== UART / STC15W ====================

static void stc15_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = STC15_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_param_config(STC15_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(STC15_UART_NUM, STC15_UART_TX_PIN, STC15_UART_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(STC15_UART_NUM, STC15_UART_BUF, 0, 0, NULL, 0));

    ESP_LOGI(TAG, "STC15W UART已初始化 (TX:GPIO%d RX:GPIO%d @ %d baud)",
             STC15_UART_TX_PIN, STC15_UART_RX_PIN, STC15_UART_BAUD);
}

static void stc15_send_frame(uint8_t addr, uint8_t cmd)
{
    // 发送节流（非阻塞：剩余等待>MAX_TX_WAIT_US则直接发送放弃节流，否则短延迟）
    int64_t now = esp_timer_get_time();
    if (last_tx_time_us > 0 && (now - last_tx_time_us) < TX_INTERVAL_US) {
        int64_t wait_us = TX_INTERVAL_US - (now - last_tx_time_us);
        if (wait_us <= MAX_TX_WAIT_US) {
            vTaskDelay(pdMS_TO_TICKS((wait_us / 1000) + 1));
        }
        // 否则等待时间过长，直接发送（放弃节流，避免主循环长时间阻塞）
    }

    uint8_t frame[6] = {FRAME_HEAD1, FRAME_HEAD2, addr, cmd, FRAME_TAIL1, FRAME_TAIL2};
    uart_write_bytes(STC15_UART_NUM, frame, sizeof(frame));
    last_tx_time_us = esp_timer_get_time();

    static const char *cmd_names_low[] = {
        "", "ON", "OFF", "QUERY", "TOGGLE",       /* 0x00-0x04 */
        "", "", "", "", "", "", "", "", "", "", ""   /* 0x05-0x0F */
    };
    static const char *cmd_names_high[] = {
        "SET_MAP", "GET_MAP"                        /* 0xA0-0xA1 */
    };
    const char *cmd_name;
    if (cmd <= 0x04)
        cmd_name = cmd_names_low[cmd];
    else if (cmd >= 0xA0 && cmd <= 0xA1)
        cmd_name = cmd_names_high[cmd - 0xA0];
    else
        cmd_name = "???";
    ESP_LOGD(TAG, "STC15W <-- 10 18 %02X %02X 18 10  (%s)", addr, cmd, cmd_name);
}

static void stc15_process_rx(void)
{
    uint8_t buf[64];
    int len = uart_read_bytes(STC15_UART_NUM, buf, sizeof(buf), 0);
    if (len <= 0) return;

    for (int i = 0; i < len; i++) {
        uint8_t dat = buf[i];

        // 映射表接收模式优先处理（跳过普通状态机，避免数据字节 0x10/0x18 干扰
if (map_rx_state != MAP_RX_IDLE) {
            // 映射表接收超时保护（最多 32条 * 4字节 = 128字节 + 帧头尾, ~150ms @9600baud
if (esp_timer_get_time() - map_rx_start_us > 200000) {
                ESP_LOGW(TAG, "映射表接收超时，重置");
                map_rx_state = MAP_RX_IDLE;
                rx_state = RX_IDLE;
                continue;
            }
            switch (map_rx_state) {
            case MAP_RX_COUNT:
                map_rx_count = dat;
                map_rx_idx = 0;
                if (map_rx_count == 0) {
                    map_rx_state = MAP_RX_TAIL1;
                } else if (map_rx_count > MAP_MAX_ENTRIES) {
                    ESP_LOGW(TAG, "映射表条数超限 %d", map_rx_count);
                    map_rx_state = MAP_RX_IDLE;
                    rx_state = RX_IDLE;  // 重置普通帧状态机，防止后续字节误判
                } else {
                    map_rx_state = MAP_RX_DATA;
                }
                break;
            case MAP_RX_DATA:
                map_rx_buf[map_rx_idx++] = dat;
                if (map_rx_idx >= map_rx_count * MAP_ENTRY_SIZE) {
                    map_rx_state = MAP_RX_TAIL1;
                }
                break;
            case MAP_RX_TAIL1:
                if (dat == FRAME_TAIL1) {
                    map_rx_state = MAP_RX_TAIL2;
                } else {
                    ESP_LOGW(TAG, "映射表帧TAIL1不匹配: %02X", dat);
                    map_rx_state = MAP_RX_IDLE;
                }
                break;
            case MAP_RX_TAIL2:
                if (dat == FRAME_TAIL2) {
                    ESP_LOGI(TAG, "STC15W --> 映射表回传 (%d条)", map_rx_count);
                    publish_1527map_resp(map_rx_buf, map_rx_count);
                } else {
                    ESP_LOGW(TAG, "映射表帧TAIL2不匹配: %02X", dat);
                }
                map_rx_state = MAP_RX_IDLE;
                map_echo_pending = false;
                rx_state = RX_IDLE;  // 防止帧尾0x10被误判为新帧头
                break;
            default:
                map_rx_state = MAP_RX_IDLE;
                break;
            }
            continue;  // 跳过普通状态机处理
        }

        // 普通帧接收超时重置（不break，不flush硬件FIFO，继续处理当前及后续字节
if (rx_state != RX_IDLE) {
            int64_t now = esp_timer_get_time();
            if (now - rx_frame_start_us > RX_TIMEOUT_US) {
                rx_state = RX_IDLE;
                rx_has_data = false;
                ESP_LOGW(TAG, "STC15W: 接收超时，重置帧状态机");
                // 不break，继续用当前dat进入RX_IDLE状态机处理
            }
        }

        switch (rx_state) {
        case RX_IDLE:
            if (dat == FRAME_HEAD1) {
                rx_state = RX_HEAD1;
                rx_frame_start_us = esp_timer_get_time();
            }
            break;
        case RX_HEAD1:
            rx_state = (dat == FRAME_HEAD2) ? RX_HEAD2 : RX_IDLE;
            break;
        case RX_HEAD2:
            rx_addr = dat;
            rx_state = RX_ADDR;
            break;
        case RX_ADDR:
            rx_cmd = dat;
            // 映射表帧: 10 18 FF A1 [count] [data...] 18 10
            if (dat == CMD_GET_MAP && rx_addr == ADDR_BROADCAST) {
                map_rx_state = MAP_RX_COUNT;
                map_rx_idx = 0;
                map_rx_count = 0;
                map_rx_start_us = esp_timer_get_time();
                map_echo_pending = false;
                rx_state = RX_IDLE;  // 退出普通状态机，进入映射表模式
                continue;  // 当前字节0xA1不进入map_rx处理
            } else {
                rx_state = RX_CMD;
            }
            break;
        case RX_CMD:
            if (dat == FRAME_TAIL1) {
                rx_has_data = false;
                rx_state = RX_TAIL1;
            } else {
                rx_data = dat;
                rx_has_data = true;
                rx_state = RX_DATA;
            }
            break;
        case RX_DATA:
            rx_state = (dat == FRAME_TAIL1) ? RX_TAIL1 : RX_IDLE;
            break;
        case RX_TAIL1:
            if (dat == FRAME_TAIL2) {
                if (rx_has_data) {
                    bool new_state = (rx_data == 0x01);
                    ESP_LOGD(TAG, "STC15W --> 10 18 %02X %02X %02X 18 10  (房间%02X 状态:%s)",
                             rx_addr, rx_cmd, rx_data, rx_addr, new_state ? "ON" : "OFF");
                    for (int j = 0; j < ROOM_COUNT; j++) {
                        if (ROOM_IDS[j] == rx_addr) {
                            if (!state_received[j] || room_states[j] != new_state) {
                                room_states[j] = new_state;
                                state_received[j] = true;
                                publish_room_state(j);
                            }
                            break;
                        }
                    }
                } else {
                    ESP_LOGD(TAG, "STC15W --> 10 18 %02X %02X 18 10  (6字节帧)", rx_addr, rx_cmd);
                }
            }
            rx_state = RX_IDLE;
            break;
        default:
            rx_state = RX_IDLE;
            break;
        }
    }
}

// ==================== OTA 升级 ====================

static void ota_task(void *pvParameter)
{
    char *url = (char *)pvParameter;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "开始OTA升级...");
    ESP_LOGI(TAG, "当前版本: %s", CURRENT_VERSION);
    ESP_LOGI(TAG, "目标固件: %s", url);
    ESP_LOGI(TAG, "========================================");

    // 重新配置看门狗为60秒超时，OTA下载可能耗时较长
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
        .skip_cert_common_name_check = true,  // 允许HTTP OTA
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);

    // 恢复看门狗为10秒超时
    wdt_config.timeout_ms = WDT_TIMEOUT_S * 1000;
    esp_task_wdt_reconfigure(&wdt_config);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA成功，重启中...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA失败: %s", esp_err_to_name(ret));
        ESP_LOGI(TAG, "继续运行当前版本");
    }

    free(url);
    vTaskDelete(NULL);
}

static void start_ota_update(const char *url)
{
    char *url_copy = strdup(url);
    if (url_copy == NULL) {
        ESP_LOGE(TAG, "OTA URL内存分配失败");
        return;
    }
    if (xTaskCreate(ota_task, "ota_task", 8192, url_copy, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "OTA任务创建失败");
        free(url_copy);
    }
}

// ==================== 主程序====================

void app_main(void)
{
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化STC15W UART
    stc15_uart_init();

    // 初始化WiFi（连接成功后自动启动MQTT）
    wifi_init();

    // LED指示
    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_2, 1);

    // 初始化看门狗 (10秒超时
    // ESP-IDF v5.5 可能已自动初始化 WDT，需先反初始化再重新配置
    esp_task_wdt_deinit();
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };
    esp_err_t wdt_ret = esp_task_wdt_init(&wdt_config);
    if (wdt_ret == ESP_OK) {
        ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
        ESP_LOGI(TAG, "看门狗已启用 (%ds)", WDT_TIMEOUT_S);
    } else {
        ESP_LOGW(TAG, "看门狗初始化失败: %s，跳过", esp_err_to_name(wdt_ret));
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "星联灯控 SL-GW1");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "设备ID: %s", DEVICE_ID);
    ESP_LOGI(TAG, "当前版本: %s", CURRENT_VERSION);
    ESP_LOGI(TAG, "MQTT: %s", MQTT_BROKER_URI);
    ESP_LOGI(TAG, "========================================");

    // 注册日志重定向: WARN/ERROR 同时发 MQTT
    s_original_vprintf = esp_log_set_vprintf(mqtt_log_vprintf);

    ESP_LOGI(TAG, "系统就绪");

    // 主循环
    while (1) {
        // 运行时WiFi连接失败，启动AP配网(60秒窗口)
        if (need_provisioning) {
            need_provisioning = false;
            start_provisioning();
            // 启动60秒倒计时，超时后切回STA无限重连
            esp_timer_create_args_t timer_args = {
                .callback = provisioning_timer_cb,
                .name = "ap_timer"
            };
            if (provisioning_timer) {
                esp_timer_stop(provisioning_timer);
                esp_timer_delete(provisioning_timer);
            }
            ESP_ERROR_CHECK(esp_timer_create(&timer_args, &provisioning_timer));
            ESP_ERROR_CHECK(esp_timer_start_once(provisioning_timer, PROVISIONING_TIMEOUT_S * 1000000ULL));
        }

        // AP配网窗口超时，关闭AP切回STA继续重连
        if (provisioning_timeout && provisioning_active) {
            provisioning_timeout = false;
            ESP_LOGI(TAG, "配网窗口结束，切回STA继续重连");
            switch_to_sta_mode();
        }

        // 广播命令延迟验证：等1.5秒后查询所有房间真实状态，纠正乐观更新可能的不一
if (verify_after_all && (esp_timer_get_time() - verify_after_all_time >= VERIFY_DELAY_US)) {
            verify_after_all = false;
            query_request = true;
        }

        // 处理查询请求（从MQTT事件/命令中设置，在主循环中统一初始化，避免竞态）
        if (query_request && mqtt_connected) {
            query_request = false;
            query_pending = true;
            query_index = 0;
            query_pass = 0;
        }

        // 逐房间查询真实状态（200ms间隔，避免HC-12半双工冲突）
        // 首轮查询6个房间，完成后补查未收到回复的房
if (query_pending && mqtt_connected) {
            if (query_index == 0 && query_pass == 0) {
                query_pass = 1;
                ESP_LOGI(TAG, "开始查询所有房间状态..");
                query_last_us = esp_timer_get_time();
            }
            if ((esp_timer_get_time() - query_last_us) >= QUERY_INTERVAL_US) {
                if (query_pass == 1 && query_index < ROOM_COUNT) {
                    // 首轮：逐房间查询（已禁用，仅用于验证）
                    // stc15_send_frame(ROOM_IDS[query_index], CMD_QUERY);
                    query_index++;
                    query_last_us = esp_timer_get_time();
                } else if (query_pass == 1 && query_index >= ROOM_COUNT) {
                    // 首轮完成，检查未回复的房间，进入补查
                    query_pass = 2;
                    query_index = 0;
                    bool any_missing = false;
                    for (int i = 0; i < ROOM_COUNT; i++) {
                        if (!state_received[i]) {
                            any_missing = true;
                            break;
                        }
                    }
                    if (!any_missing) {
                        query_pending = false;
                        query_index = 0;
                        query_pass = 0;
                        ESP_LOGI(TAG, "所有房间状态已获取");
                    } else {
                        ESP_LOGI(TAG, "首轮查询完成，补查未回复房间...");
                        query_last_us = esp_timer_get_time();
                    }
                } else if (query_pass == 2) {
                    // 补查：找到下一个未收到回复的房间
                    bool found = false;
                    while (query_index < ROOM_COUNT) {
                        if (!state_received[query_index]) {
                            stc15_send_frame(ROOM_IDS[query_index], CMD_QUERY);
                            query_index++;
                            query_last_us = esp_timer_get_time();
                            found = true;
                            break;
                        }
                        query_index++;
                    }
                    if (!found) {
                        query_pending = false;
                        query_index = 0;
                        query_pass = 0;
                        ESP_LOGI(TAG, "补查完成");
                    }
                }
            }
        }

        stc15_process_rx();

        // 映射表回传等待超时保护（STC15处理+回传 ~200ms，预留 500ms
if (map_echo_pending && (esp_timer_get_time() - map_echo_sent_us > 500000)) {
            map_echo_pending = false;
            ESP_LOGW(TAG, "映射表回传等待超时");
        }

        // 心跳上报
        if (mqtt_connected) {
            int64_t now = esp_timer_get_time();
            if (now - last_heartbeat_us >= (int64_t)HEARTBEAT_INTERVAL_MS * 1000) {
                last_heartbeat_us = now;
                publish_device_info();
            }
        }

        // 喂狗
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/*
 * ============================================
 *  更新日志 (Changelog)
 * ============================================
 *
 * v1.0.6 (当前版本)
 * ----------------
 *
 * 【新增功能】
 *   - 上电智能连网: 有凭据直接连WiFi，连不上5次才弹AP；无凭据直接AP
 *   - AP仅一次: 一次上电只开一次AP(60s窗口)，之后无限重连不弹AP
 *   - ALLON/ALLOFF乐观更新: 广播后立即更新状态上报MQTT，HA秒响应
 *   - ALLON/ALLOFF延迟验证: 广播后1.5s自动查询真实状态纠错
 *   - 单房间乐观更新: 点按后HA即时响应，等STC15W回复确认
 *   - 远程日志上报: WARN/ERROR自动发MQTT，HA自动发现"星联灯控日志"
 *                    MQTT上下文内日志缓存，延迟上报，无重入风险
 *
 * 【优化】
 *   - 移除上电自动查询: MQTT连接后不再主动查房间状态
 *   - 精简日志: 帧收发日志改为DEBUG级别，默认静默
 *   - WiFi扫描加速: 每信道60~100ms
 *   - HTTP服务器优化: socket 7->12 + LRU淘汰
 *   - 心跳初值修正: 连接后等60s才首报
 *
 * 【Bug修复】
 *   - 配网esp_wifi_deinit后未重建netif -> AP网页/DNS失效
 *   - OTA xTaskCreate失败 -> 内存泄漏
 *   - MQTT遗嘱msg_len=0 -> 显式指定
 *   - WiFi扫描ESP_ERROR_CHECK -> 崩溃风险
 *   - provisioning_timer_cb 重复定义
 *   - 注释过时/引脚号不匹配
 *   - MQTT日志重入保护 -> 加in_mqtt_context标志+环形缓冲
 *
 */