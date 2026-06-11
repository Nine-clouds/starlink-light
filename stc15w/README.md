# STC15W 桥接模块

4 路串口 + 1527 RF 解码 + HC-12 无线通信。

## 硬件

- **MCU**: STC15W4K32S4, 内部 IRC 22.1184MHz
- **UART1** (P3.6/RxD1, P3.7/TxD1): CI-03T 语音识别模块
- **UART2** (P1.0/RxD2, P1.1/TxD2): ESP32-C3 通信
- **UART3** (P0.0/RxD3, P0.1/TxD3): HC-12 无线模块
- **UART4** (P0.2/RxD4): 433MHz 1527 解码模块（只接收）
- **LED**: P1.7

## 功能

- **UART 透传**: ESP32 ↔ HC-12 双向转发
- **1527 解码**: 遥控器信号 → 查映射表 → 发灯控命令
- **映射表管理**: 支持 ESP32 远程读写，EEPROM 断电保存
- **广播命令**: 全开/全关，所有从机响应
- **看门狗**: ~4.5s 超时自动复位

## 部署

1. 打开 `starlink-stc15.uvproj`（Keil C51）
2. 编译 → 烧录到 STC15W4K32S4

## 协议

帧格式: `10 18 [地址] [命令] 18 10`

| 命令 | 值 | 说明 |
|------|-----|------|
| ON | 0x01 | 开灯 |
| OFF | 0x02 | 关灯 |
| QUERY | 0x03 | 查询状态 |
| TOGGLE | 0x04 | 翻转 |
| SET_MAP | 0xA0 | 写入映射表 |
| GET_MAP | 0xA1 | 读取映射表 |

## 映射表帧格式

**写入** (ESP32 → STC15):
`10 18 FF A0 [count] [addr_h addr_l key dev_addr]×N 18 10`

**读取** (STC15 → ESP32):
`10 18 FF A1 [count] [addr_h addr_l key dev_addr]×N 18 10`

## 文件

| 文件 | 说明 |
|------|------|
| `main.c` | 主程序 |
| `STC15Fxxxx.H` | STC15 寄存器头文件 |
| `starlink-stc15.uvproj` | Keil 工程 |
