// 将本文件重命名为 config.h，填入你的真实信息后编译

// WiFi 配网默认凭据（可选，留空则上电直接开 AP 等待配网）
#define WIFI_CONFIG_SSID     "你的WiFi名"
#define WIFI_CONFIG_PASS     "你的WiFi密码"

// MQTT 服务器
#define MQTT_BROKER_URI      "mqtt://你的IP:端口"
#define MQTT_USERNAME        "用户名"
#define MQTT_PASSWORD        "密码"

// 设备标识
#define DEVICE_ID            "starlink_gw1"

// OTA 固件地址
#define FW_BASE_URL          "http://你的IP:端口/firmware"
#define DEFAULT_FW_URL       FW_BASE_URL "/latest/firmware.bin"
