# 1527 RF 映射表管理

HTTP → MQTT 桥接，浏览器远程管理 ESP32 网关上的 1527 RF 遥控器映射表。

## 架构

```
浏览器 ──HTTP──▶ 1527 Mapper ──MQTT──▶ ESP32 网关 ──UART──▶ STC15W（写入映射表）
    │                  │
    └── SSE 实时推送 ──┘ (home/gateway/1527map_resp)
```

## 功能

- **查看映射表**: 从 ESP32 读取当前 1527 映射
- **添加/编辑/删除映射**: 通过 Web 界面管理
- **SSE 实时推送**: MQTT 响应实时显示
- **双语言**: Python (Flask) / Node.js (Express)

## 部署

### Python 版

```bash
cd mapper/
cp .env.example .env        # 编辑 MQTT Broker 信息
pip3 install flask paho-mqtt
python3 1527_mapper_server.py
# 监听 0.0.0.0:18597
```

### Node.js 版

```bash
cd mapper/
npm install
cp .env.example .env        # 编辑 MQTT 信息
node 1527_mapper_server.js
# 监听 0.0.0.0:18597
```

### 配置 (.env)

```env
MQTT_BROKER=你的MQTT服务器IP
MQTT_PORT=1883
MQTT_USER=用户名
MQTT_PASS=密码
```

## 使用

1. 启动服务器
2. 浏览器访问 `http://服务器IP:18597/`
3. 点击"加载映射表"读取当前数据
4. 编辑完成后点击"保存"写入 ESP32

## API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/status` | MQTT 连接状态 |
| POST | `/api/send` | 下发指令（写入映射） |
| GET | `/api/load` | 请求当前映射表 |
| GET | `/api/subscribe` | SSE 事件流 |

## MQTT 主题

| 主题 | 方向 | 说明 |
|------|------|------|
| `home/gateway/1527map` | Server → Gateway | 下发指令 |
| `home/gateway/1527map_resp` | Gateway → Server | 网关响应 |

## 数据格式

映射表条目（JSON）：

```json
{
  "rf_addr": "682D",   // RF 地址（4位 hex）
  "rf_key": "34",      // RF 键值（2位 hex）
  "dev_addr": "01"     // 设备地址（2位 hex，对应房间号 0x01~0x06）
}
```

## 文件

| 文件 | 说明 |
|------|------|
| `1527_mapper_server.py` | Python 版服务端 |
| `1527_mapper_server.js` | Node.js 版服务端 |
| `1527_mapper.html` | Web 管理界面 |
| `.env.example` | 配置模板 |
