/**
 * ESP32 智能灯控网关 (ESP-IDF)
 *
 * 架构: Home Assistant ──MQTT──▶ESP32 ──UART──▶STC15W ──HC-12/RF──▶灯控
 *
 * 本文件是主程序入口，负责:
 *   - 初始化各模块
 *   - 主循环协调
 *
 * 模块划分:
 *   - uart_protocol.c - STC15W串口协议
 *   - wifi_prov.c    - WiFi配网
 *   - mqtt_ha.c      - MQTT和HA发现
 *   - gateway_state.c - 全局状态
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "config.h"
#include "gateway_state.h"
#include "uart_protocol.h"
#include "wifi_prov.h"
#include "mqtt_ha.h"

static const char *TAG = "gateway";

// ==================== 主程序 ====================

void app_main(void)
{
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化全局状态
    gateway_state_init();

    // 初始化STC15W UART
    stc15_uart_init();

    // 初始化WiFi（连接成功后自动启动MQTT）
    wifi_init();

    // LED指示
    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_2, 1);

    // 初始化看门狗 (10秒超时)
    esp_task_wdt_deinit();
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };
    esp_err_t wdt_ret = esp_task_wdt_init(&wdt_config);
    if (wdt_ret == ESP_OK) {
        ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
        ESP_LOGI(TAG, "Watchdog enabled (%ds)", WDT_TIMEOUT_S);
    } else {
        ESP_LOGW(TAG, "Watchdog init failed: %s, skip", esp_err_to_name(wdt_ret));
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "星联灯控 SL-GW1");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Device ID: %s", DEVICE_ID);
    ESP_LOGI(TAG, "Current Version: %s", CURRENT_VERSION);
    ESP_LOGI(TAG, "MQTT: %s", MQTT_BROKER_URI);
    ESP_LOGI(TAG, "========================================");

    // 注册日志重定向: WARN/ERROR 同时发 MQTT
    // TODO: 需要导出 s_original_vprintf 或在 mqtt_ha 模块中提供函数
    // s_original_vprintf = esp_log_set_vprintf(mqtt_log_vprintf);

    ESP_LOGI(TAG, "System Ready");

    // 主循环
    while (1) {
        // 运行时WiFi连接失败，启动AP配网(60秒窗口)
        if (need_provisioning) {
            need_provisioning = false;
            start_provisioning();
            // 启动60秒倒计时
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
            ESP_LOGI(TAG, "Provisioning window ended, switch to STA");
            switch_to_sta_mode();
        }

        // 广播命令延迟验证
        if (verify_after_all && (esp_timer_get_time() - verify_after_all_time >= VERIFY_DELAY_US)) {
            verify_after_all = false;
            query_request = true;
        }

        // 处理查询请求
        if (query_request && mqtt_connected) {
            query_request = false;
            query_pending = true;
            query_index = 0;
            query_pass = 0;
        }

        // 逐房间查询真实状态
        if (query_pending && mqtt_connected) {
            if (query_index == 0 && query_pass == 0) {
                query_pass = 1;
                ESP_LOGI(TAG, "Start querying all rooms...");
                query_last_us = esp_timer_get_time();
            }
            if ((esp_timer_get_time() - query_last_us) >= QUERY_INTERVAL_US) {
                if (query_pass == 1 && query_index < ROOM_COUNT) {
                    query_index++;
                    query_last_us = esp_timer_get_time();
                } else if (query_pass == 1 && query_index >= ROOM_COUNT) {
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
                        ESP_LOGI(TAG, "All rooms state received");
                    } else {
                        ESP_LOGI(TAG, "First pass done, re-query missing...");
                        query_last_us = esp_timer_get_time();
                    }
                } else if (query_pass == 2) {
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
                        ESP_LOGI(TAG, "Re-query done");
                    }
                }
            }
        }

        // 处理STC15W接收
        stc15_process_rx();

        // 映射表回传等待超时保护
        // TODO: 需要从 uart_protocol 模块获取状态
        // if (stc15_is_map_echo_pending() && (esp_timer_get_time() - stc15_get_map_echo_sent_us() > 500000)) {
        //     stc15_set_map_echo_pending(false);
        //     ESP_LOGW(TAG, "Map echo timeout");
        // }

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
