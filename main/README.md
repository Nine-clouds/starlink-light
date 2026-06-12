# ESP32-C3 网关

WiFi + MQTT + 配网 + OTA，桥接 Home Assistant 与 STC15W。

## 功能

- **WiFi 配网**: Captive Portal + DNS 劫持，手机连 AP 自动弹页面
- **MQTT 通信**: 自动发现 HA，6 房间独立开关 + 全开全关
- **乐观更新**: 点击即时响应，1.5s 后自动查询纠错
- **STC15W 桥接**: UART 指令收发，协议转换
- **1527 映射表**: 通过 MQTT 远程读写 RF 遥控器映射
- **OTA 升级**: 远程更新固件
- **日志上报**: WARN/ERROR → MQTT → HA
- **看门狗**: 10s 超时自动复位

## 部署

### 1. 配置

```bash
cp config.example.h config.h
```

编辑 `config.h`：

```c
#define MQTT_BROKER_URI    "mqtt://你的IP:端口"
#define MQTT_USERNAME      "用户名"
#define MQTT_PASSWORD      "密码"
#define FW_BASE_URL        "http://你的IP:端口/firmware"
```

### 2. 编译烧录

```bash
idf.py build
idf.py flash
idf.py monitor    # 查看日志
```

## 启动流程

1. 上电 → NVS 读取 WiFi 凭据
2. 有凭据 → STA 连接（连不上 5 次弹 AP 60s）
3. 无凭据 → 开 AP 等待配网
4. WiFi 获取 IP → MQTT 连接 → 发送 HA 自动发现
5. 主循环: UART 收发 / 查询 / MQTT / 心跳 / 喂狗

## 引脚

| GPIO | 功能 |
|------|------|
| 6 | UART RX (← STC15W TX) |
| 7 | UART TX (→ STC15W RX) |
| 2 | LED |

## 模块架构

| 文件 | 说明 |
|------|------|
| `esp32_gateway_main.c` | 主程序入口（初始化 + 主循环） |
| `gateway_state.c/h` | 全局共享状态（房间配置、MQTT/WiFi状态、查询队列） |
| `mqtt_ha.c/h` | MQTT客户端、HA自动发现、OTA升级、日志上报 |
| `uart_protocol.c/h` | STC15W串口协议收发、1527映射表 |
| `wifi_prov.c/h` | WiFi配网、AP模式、HTTP配置页、DNS劫持 |
| `config.h` | 私有配置（不入库） |
| `config.example.h` | 配置模板 |
