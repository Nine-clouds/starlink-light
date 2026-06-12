/**
 * WiFi配网模块
 *
 * 负责:
 *   - WiFi STA/AP模式切换
 *   - 智能连网：有凭据直连，连不上5次弹AP；无凭据直接AP
 *   - AP配网页面（Captive Portal）：DNS劫持 + HTTP配置页
 *   - WiFi扫描结果返回给前端
 *   - NVS存储WiFi凭据
 *   - 配网超时后自动切回STA模式
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
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "wifi_prov.h"
#include "gateway_state.h"
#include "mqtt_ha.h"
#include "config.h"

static const char *TAG = "wifi_prov";

// ==================== DNS劫持服务（Captive Portal） ====================
// 将所有DNS请求解析到192.168.4.1，使手机自动跳转配网页面
static int dns_sock = -1;
static volatile bool dns_running = false;
static TaskHandle_t dns_task_handle = NULL;

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
    "<button class='submit-btn' onclick='save()'>保存并连接</button>"
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

static void dns_task(void *pvParameters)
{
    dns_running = true;

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(53);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    dns_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (dns_sock < 0) {
        ESP_LOGE(TAG, "DNS: socket create failed");
        dns_running = false;
        vTaskDelete(NULL);
        return;
    }

    int optval = 1;
    setsockopt(dns_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (bind(dns_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS: bind failed (errno=%d)", errno);
        close(dns_sock);
        dns_sock = -1;
        dns_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS hijack server started (all domains -> 192.168.4.1)");

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
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (!dns_running) break;
            continue;
        }
        if (recv_len < 12 || recv_len > 240) continue;

        uint8_t tx_buf[256];
        int tx_len = recv_len;

        memcpy(tx_buf, rx_buf, recv_len);

        tx_buf[2] = 0x81;
        tx_buf[3] = 0x80;
        tx_buf[6] = 0x00; tx_buf[7] = 0x01;

        int pos = recv_len;
        tx_buf[pos++] = 0xC0; tx_buf[pos++] = 0x0C;
        tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x01;
        tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x01;
        tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x3C;
        tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x04;
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
    if (dns_task_handle) {
        for (int i = 0; i < 10 && eTaskGetState(dns_task_handle) != eDeleted; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        dns_task_handle = NULL;
    }
}

// ==================== 强制跳转（Captive Portal） ====================
// 404页面重定向到配网页，配合DNS劫持实现自动弹出

static esp_err_t captive_portal_handler(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ==================== WiFi配置HTTP处理 ====================

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
        return ESP_OK;
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
            return ESP_FAIL;
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

    ESP_LOGI(TAG, "WiFi config received: SSID=%s", ssid);

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
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
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

httpd_handle_t start_config_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = 12;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return NULL;
    }

    httpd_uri_t get_uri = { .uri = "/", .method = HTTP_GET, .handler = config_get_handler };
    httpd_uri_t post_uri = { .uri = "/save", .method = HTTP_POST, .handler = config_post_handler };
    httpd_uri_t scan_uri = { .uri = "/scan", .method = HTTP_GET, .handler = config_scan_handler };

    httpd_register_uri_handler(server, &get_uri);
    httpd_register_uri_handler(server, &post_uri);
    httpd_register_uri_handler(server, &scan_uri);

    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, captive_portal_handler);

    dns_server_start();

    ESP_LOGI(TAG, "HTTP config server + DNS started (Captive Portal)");
    return server;
}

void stop_config_server(void)
{
    dns_server_stop();
    if (config_server) {
        httpd_stop(config_server);
        config_server = NULL;
    }
}

// ==================== NVS WiFi凭据存储 ====================

esp_err_t save_wifi_credentials(const char *ssid, const char *password)
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

esp_err_t load_wifi_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len)
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

// ==================== WiFi事件处理 ====================
// STA模式: 连接失败5次 → 弹AP配网（仅一次）
// AP模式: 60秒超时 → 切回STA模式

void wifi_event_handler(void *arg, esp_event_base_t event_base,
                       int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            if (!provisioning_active) {
                ESP_LOGI(TAG, "WiFi STA started, connecting...");
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            wifi_connected = false;
            if (provisioning_active) break;
            s_retry_num++;
            if (s_retry_num <= WIFI_MAX_RETRY) {
                ESP_LOGI(TAG, "WiFi reconnecting... (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
                esp_wifi_connect();
            } else if (!ap_attempted) {
                ESP_LOGW(TAG, "WiFi connect failed %d times, start provisioning (%ds)", WIFI_MAX_RETRY, PROVISIONING_TIMEOUT_S);
                ap_attempted = true;
                need_provisioning = true;
            } else {
                ESP_LOGW(TAG, "WiFi connect failed, continue retry...");
                s_retry_num = 0;
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_CONNECTED:
            s_retry_num = 0;
            ESP_LOGI(TAG, "WiFi associated to AP");
            break;
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "Provisioning AP started: %s", AP_SSID);
            {
                esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
                if (ap_netif) {
                    uint32_t dns_ip = ipaddr_addr("192.168.4.1");
                    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                        ESP_NETIF_DOMAIN_NAME_SERVER, &dns_ip, sizeof(dns_ip));
                    ESP_LOGI(TAG, "DHCP DNS option set: 192.168.4.1");
                }
            }
            if (!config_server) {
                config_server = start_config_server();
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "Device connected to provisioning AP");
            break;
        case WIFI_EVENT_AP_STOP:
            if (config_server) {
                stop_config_server();
            }
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
        provisioning_active = false;
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (!mqtt_client) {
            mqtt_app_start();
        }
    }
}

void provisioning_timer_cb(void *arg)
{
    provisioning_timeout = true;
}

void start_provisioning(void)
{
    if (provisioning_active) return;
    provisioning_active = true;
    s_retry_num = 0;

    if (mqtt_client) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        mqtt_connected = false;
        ESP_LOGI(TAG, "MQTT client stopped");
    }

    stop_config_server();

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    vTaskDelay(pdMS_TO_TICKS(500));

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, AP_SSID, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(AP_SSID);
    ap_config.ap.max_connection = AP_MAX_CONN;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    strlcpy((char *)ap_config.ap.password, AP_PASSWORD, sizeof(ap_config.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
    wifi_config_t sta_blank = {0};
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_blank));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connect to WiFi '%s' (password %s) and visit http://192.168.4.1", AP_SSID, AP_PASSWORD);
}

void switch_to_sta_mode(void)
{
    if (provisioning_timer) {
        esp_timer_stop(provisioning_timer);
        esp_timer_delete(provisioning_timer);
        provisioning_timer = NULL;
    }

    char ssid[33] = {0}, password[65] = {0};
    esp_err_t err = load_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password));
    if (err != ESP_OK || strlen(ssid) == 0) {
        ESP_LOGW(TAG, "Timeout but no WiFi credentials, keep AP mode");
        return;
    }

    ESP_LOGI(TAG, "Provisioning window ended, switch to STA mode: %s", ssid);

    stop_config_server();

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    vTaskDelay(pdMS_TO_TICKS(500));

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta_netif, "starlink-gw1");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    provisioning_active = false;
    provisioning_timeout = false;
}

void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

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
        wifi_config_t wifi_config = {0};
        strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "Trying to connect WiFi: %s", ssid);
    } else {
        ESP_LOGI(TAG, "No WiFi credentials, start provisioning");
        start_provisioning();
    }
}
