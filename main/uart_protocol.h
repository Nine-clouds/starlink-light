/**
 * UART协议模块 - STC15W通信
 *
 * 帧格式: 10 18 [ADDR] [CMD] [DATA] 18 10
 * 波特率: 9600, TX:GPIO7, RX:GPIO6
 */

#ifndef UART_PROTOCOL_H
#define UART_PROTOCOL_H

#include "driver/uart.h"

// ==================== STC15W Frame Protocol ====================
#define FRAME_HEAD1        0x10
#define FRAME_HEAD2        0x18
#define FRAME_TAIL1        0x18
#define FRAME_TAIL2        0x10
#define ADDR_BROADCAST     0xFF

// Commands
#define CMD_ON             0x01
#define CMD_OFF            0x02
#define CMD_QUERY          0x03
#define CMD_TOGGLE         0x04
#define CMD_SET_MAP        0xA0    // Write 1527 mapping
#define CMD_GET_MAP        0xA1    // Read 1527 mapping

#define MAP_ENTRY_SIZE     4       // Each mapping: addr_h + addr_l + key + dev_addr
#define MAP_MAX_ENTRIES    32      // Max mapping entries

// ==================== UART Configuration ====================
#define STC15_UART_NUM     UART_NUM_1
#define STC15_UART_TX_PIN  7
#define STC15_UART_RX_PIN  6
#define STC15_UART_BAUD    9600
#define STC15_UART_BUF     256

// ==================== Function Declarations ====================

/**
 * Initialize UART for STC15W communication
 */
void stc15_uart_init(void);

/**
 * Send a frame to STC15W
 * @param addr Target address (0x01-0x06 for rooms, 0xFF for broadcast)
 * @param cmd Command to send
 */
void stc15_send_frame(uint8_t addr, uint8_t cmd);

/**
 * Process received data from STC15W
 * Should be called in main loop
 */
void stc15_process_rx(void);

#endif // UART_PROTOCOL_H
