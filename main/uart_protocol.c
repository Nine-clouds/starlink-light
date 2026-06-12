/**
 * UART协议模块 - STC15W通信
 *
 * 负责:
 *   - STC15W串口初始化（UART1, 9600baud）
 *   - 帧发送：带发送节流（80ms间隔，超50ms放弃等待）
 *   - 帧接收：状态机解析，支持6字节帧和7字节帧
 *   - 1527映射表接收：多帧解析，带超时保护
 *   - 帧格式: 10 18 [ADDR] [CMD] [DATA] 18 10
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "uart_protocol.h"
#include "gateway_state.h"

static const char *TAG = "uart_protocol";

// ==================== STC15W接收状态机 ====================
// 状态: RX_IDLE → RX_HEAD1 → RX_HEAD2 → RX_ADDR → RX_CMD → RX_DATA/RX_TAIL1 → RX_TAIL1
typedef enum {
    RX_IDLE, RX_HEAD1, RX_HEAD2, RX_ADDR, RX_CMD, RX_DATA, RX_TAIL1
} rx_state_t;

static rx_state_t rx_state = RX_IDLE;
static uint8_t rx_addr, rx_cmd, rx_data;
static bool rx_has_data = false;
static int64_t rx_frame_start_us = 0;

// ==================== STC15W发送节流 ====================
// 两次发送间隔至少80ms，等待超过50ms则放弃等待直接发送
static int64_t last_tx_time_us = 0;

// ==================== 1527映射表接收缓冲 ====================
// 映射表帧格式: 10 18 FF A1 [count] [data...] 18 10
// 每条映射4字节: addr_h + addr_l + rf_key + dev_addr
typedef enum {
    MAP_RX_IDLE, MAP_RX_COUNT, MAP_RX_DATA, MAP_RX_TAIL1, MAP_RX_TAIL2
} map_rx_state_t;

static map_rx_state_t map_rx_state = MAP_RX_IDLE;
static uint8_t map_rx_count = 0;
static uint8_t map_rx_buf[MAP_MAX_ENTRIES * MAP_ENTRY_SIZE];
static uint8_t map_rx_idx = 0;
static int64_t map_rx_start_us = 0;    // Mapping table receive start time
static bool map_echo_pending = false;  // Waiting for STC15 to echo mapping
static int64_t map_echo_sent_us = 0;   // Mapping command send time

// ==================== 外部变量声明 ====================
// 定义在 gateway_state.c 中
extern bool room_states[];
extern bool state_received[];
extern const uint8_t ROOM_IDS[];

void stc15_uart_init(void)
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

    ESP_LOGI(TAG, "STC15W UART initialized (TX:GPIO%d RX:GPIO%d @ %d baud)",
             STC15_UART_TX_PIN, STC15_UART_RX_PIN, STC15_UART_BAUD);
}

void stc15_send_frame(uint8_t addr, uint8_t cmd)
{
    // Send throttling (non-blocking: if wait > MAX_TX_WAIT_US, send directly)
    int64_t now = esp_timer_get_time();
    if (last_tx_time_us > 0 && (now - last_tx_time_us) < TX_INTERVAL_US) {
        int64_t wait_us = TX_INTERVAL_US - (now - last_tx_time_us);
        if (wait_us <= MAX_TX_WAIT_US) {
            vTaskDelay(pdMS_TO_TICKS((wait_us / 1000) + 1));
        }
        // Otherwise wait time too long, send directly (abort throttling)
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

void stc15_process_rx(void)
{
    uint8_t buf[64];
    int len = uart_read_bytes(STC15_UART_NUM, buf, sizeof(buf), 0);
    if (len <= 0) return;

    for (int i = 0; i < len; i++) {
        uint8_t dat = buf[i];

        // Mapping table receive mode takes priority (skip normal state machine)
        if (map_rx_state != MAP_RX_IDLE) {
            // Mapping table receive timeout protection
            if (esp_timer_get_time() - map_rx_start_us > 200000) {
                ESP_LOGW(TAG, "Mapping table receive timeout, reset");
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
                    ESP_LOGW(TAG, "Mapping table count exceeded %d", map_rx_count);
                    map_rx_state = MAP_RX_IDLE;
                    rx_state = RX_IDLE;
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
                    ESP_LOGW(TAG, "Mapping table frame TAIL1 mismatch: %02X", dat);
                    map_rx_state = MAP_RX_IDLE;
                }
                break;
            case MAP_RX_TAIL2:
                if (dat == FRAME_TAIL2) {
                    ESP_LOGI(TAG, "STC15W --> Mapping table echoed (%d entries)", map_rx_count);
                    // TODO: Publish mapping response to MQTT
                    // publish_1527map_resp(map_rx_buf, map_rx_count);
                } else {
                    ESP_LOGW(TAG, "Mapping table frame TAIL2 mismatch: %02X", dat);
                }
                map_rx_state = MAP_RX_IDLE;
                map_echo_pending = false;
                rx_state = RX_IDLE;
                break;
            default:
                map_rx_state = MAP_RX_IDLE;
                break;
            }
            continue;
        }

        // Normal frame receive timeout reset
        if (rx_state != RX_IDLE) {
            int64_t now = esp_timer_get_time();
            if (now - rx_frame_start_us > RX_TIMEOUT_US) {
                rx_state = RX_IDLE;
                rx_has_data = false;
                ESP_LOGW(TAG, "STC15W: Receive timeout, reset frame state machine");
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
            // Mapping table frame: 10 18 FF A1 [count] [data...] 18 10
            if (dat == CMD_GET_MAP && rx_addr == ADDR_BROADCAST) {
                map_rx_state = MAP_RX_COUNT;
                map_rx_idx = 0;
                map_rx_count = 0;
                map_rx_start_us = esp_timer_get_time();
                map_echo_pending = false;
                rx_state = RX_IDLE;
                continue;
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
                    ESP_LOGD(TAG, "STC15W --> 10 18 %02X %02X %02X 18 10  (Room%02X State:%s)",
                             rx_addr, rx_cmd, rx_data, rx_addr, new_state ? "ON" : "OFF");
                    for (int j = 0; j < ROOM_COUNT; j++) {
                        if (ROOM_IDS[j] == rx_addr) {
                            if (!state_received[j] || room_states[j] != new_state) {
                                room_states[j] = new_state;
                                state_received[j] = true;
                                // TODO: Publish room state to MQTT
                                // publish_room_state(j);
                            }
                            break;
                        }
                    }
                } else {
                    ESP_LOGD(TAG, "STC15W --> 10 18 %02X %02X 18 10  (6-byte frame)", rx_addr, rx_cmd);
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

// ==================== 映射表状态查询接口 ====================

bool stc15_is_map_echo_pending(void)
{
    return map_echo_pending;
}

int64_t stc15_get_map_echo_sent_us(void)
{
    return map_echo_sent_us;
}

void stc15_set_map_echo_pending(bool pending)
{
    map_echo_pending = pending;
    if (pending) {
        map_echo_sent_us = esp_timer_get_time();
    }
}
