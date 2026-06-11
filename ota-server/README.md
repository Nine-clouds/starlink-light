# HTTP OTA 固件服务器

固件托管 + Web 管理 + MQTT 推送升级。

## 架构

```
浏览器 ──HTTP──▶ OTA Server ──MQTT──▶ ESP32 网关
   │                 │                      │
   │ 上传固件        │ 推送升级通知         └── HTTP 下载固件 ──▶ OTA Server
   │                 │
   ├── API 查询版本
   └── Web 管理界面
```

## 功能

- **固件上传/删除**: Web 界面拖拽上传
- **版本管理**: 自动维护 manifest.json
- **断点续传**: 支持 Range 请求，大文件下载不中断
- **MQTT 推送**: 新固件发布后通知设备升级
- **设备状态**: 在线设备列表
- **Token 认证**: 可选的 API 安全认证
- **下载日志**: 记录每次固件下载

## 部署

### 1. 安装依赖

```bash
cd ota-server/
pip3 install paho-mqtt
```

### 2. 配置

```bash
cp .env.example .env
```

编辑 `.env`（MQTT 可选）：

```env
MQTT_BROKER=你的MQTT服务器IP
MQTT_PORT=1883
MQTT_USER=用户名
MQTT_PASSWORD=密码
OTA_SERVER_PORT=15678
```

### 3. 放置固件

```bash
mkdir -p firmware/latest
cp 你的固件.bin firmware/latest/firmware.bin
```

### 4. 启动

```bash
python3 server.py
# 监听 0.0.0.0:15678
```

## 使用

### Web 界面

浏览器访问 `http://服务器IP:15678/`：
- 上传新固件
- 查看服务器状态
- 推送到设备

### 命令行上传

```bash
python3 upload_tool.py --file firmware.bin --version v1.2.0
```

### MQTT 推送升级

访问 Web 界面 → 点击"推送升级"，或 POST：

```bash
curl -X POST http://服务器IP:15678/api/mqtt/push \
  -H "Content-Type: application/json" \
  -d '{"target":"all","version":"v1.2.0"}'
```

ESP32 收到 `ota/upgrade/command` 后自动下载并升级。

## API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/version` | 最新版本信息 |
| GET | `/api/manifest` | 全部版本清单 |
| GET | `/api/status` | 服务器状态 |
| GET | `/api/logs` | 下载日志 |
| POST | `/api/upload` | 上传固件（multipart） |
| POST | `/api/delete` | 删除指定版本 |
| POST | `/api/mqtt/push` | MQTT 推送升级通知 |
| GET | `/api/mqtt/status` | MQTT 连接 / 设备列表 |
| GET | `/firmware/{version}/firmware.bin` | 固件下载 |

## 目录结构

```
ota-server/
├── server.py                主服务器
├── server_mqtt.py           MQTT 推送服务
├── device_client.py         设备端 OTA 客户端示例
├── upload_tool.py           命令行上传工具
├── config.json              服务器配置
├── manifest.json            固件版本清单（自动维护）
├── firmware/                固件存储
│   └── latest/firmware.bin  最新固件
├── templates/index.html     Web 管理界面
├── .env.example             配置模板
└── .gitignore
```

## MQTT 主题

| 主题 | 方向 | 说明 |
|------|------|------|
| `ota/upgrade/command` | Server → ESP32 | 升级指令 |
| `ota/device/status` | ESP32 → Server | 设备状态上报 |

## License

MIT
