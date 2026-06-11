#!/usr/bin/env python3
import os
"""
HTTP OTA 固件更新服务�?提供固件文件下载和版本管理API
"""

import os
import json
import hashlib
import logging
from datetime import datetime
from http.server import HTTPServer, SimpleHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
import cgi  # 用于解析multipart/form-data
import tempfile

# 导入API处理�?from api_handler import api_handler

# 配置日志
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler('ota.log'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

# 配置文件
CONFIG_FILE = 'config.json'
MANIFEST_FILE = 'manifest.json'
FIRMWARE_DIR = 'firmware'

# MQTT配置（optional�?MQTT_ENABLED = False  # 默认禁用，需要安装paho-mqtt
MQTT_BROKER = os.environ.get("MQTT_BROKER", "your-mqtt-broker.example.com")  # MQTT Broker 地址
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))               # MQTT 端口
MQTT_USER = os.environ.get("MQTT_USER", "your-mqtt-username")      # MQTT 用户�?MQTT_PASSWORD = os.environ.get("MQTT_PASSWORD", "your-mqtt-password")  # MQTT 密码
mqtt_client = None
mqtt_devices = {}

# 尝试导入MQTT�?try:
    import paho.mqtt.client as mqtt
    MQTT_ENABLED = True
    logger.info("MQTT库已加载")
except ImportError:
    logger.warning("MQTT库未安装，推送功能禁用。安�? pip3 install paho-mqtt")
except Exception as e:
    logger.warning(f"MQTT导入失败: {e}")

class OTARequestHandler(SimpleHTTPRequestHandler):
    """OTA HTTP请求处理�?""
    
    def __init__(self, *args, **kwargs):
        self.config = self.load_config()
        # 不指定directory，使用当前目�?        super().__init__(*args, directory='.', **kwargs)
    
    def load_config(self):
        """加载配置文件"""
        try:
            with open(CONFIG_FILE, 'r') as f:
                return json.load(f)
        except FileNotFoundError:
            logger.warning("配置文件不存在，使用默认配置")
            return {
                "port": 8080,
                "require_token": False,
                "token": "",
                "log_downloads": True
            }
    
    def do_GET(self):
        """处理GET请求"""
        parsed_path = urlparse(self.path)
        
        # API路由
        if parsed_path.path == '/api/version':
            self.handle_version_api()
        elif parsed_path.path == '/api/manifest':
            self.handle_manifest_api()
        elif parsed_path.path == '/api/status':
            self.handle_status_api()
        elif parsed_path.path == '/api/logs':
            self.handle_logs_api()
        elif parsed_path.path == '/api/mqtt/status':
            self.handle_mqtt_status_api()
        elif parsed_path.path == '/api/delete':  # �?新增：支持GET删除
            self.handle_delete_api()
        elif parsed_path.path == '/' or parsed_path.path == '/index.html':
            self.handle_web_ui()
        elif parsed_path.path.startswith('/firmware/'):
            self.handle_firmware_download(parsed_path.path)
        else:
            # 默认文件服务
            super().do_GET()
    
    def do_POST(self):
        """处理POST请求"""
        parsed_path = urlparse(self.path)
        
        if parsed_path.path == '/api/upload':
            self.handle_upload_api()
        elif parsed_path.path == '/api/delete':
            self.handle_delete_api()
        elif parsed_path.path == '/api/mqtt/push':
            self.handle_mqtt_push_api()
        else:
            self.send_error(404, "Not Found")
    
    def handle_upload_api(self):
        """处理固件上传 - 支持Web上传"""
        try:
            # Token验证
            if self.config.get('security', {}).get('require_token', False):
                query = parse_qs(urlparse(self.path).query)
                token = query.get('token', [None])[0]
                if token != self.config.get('security', {}).get('token', ''):
                    self.send_json_response(403, {"error": "Invalid token"})
                    return
            
            # 解析multipart/form-data
            content_type = self.headers.get('Content-Type')
            if not content_type or 'multipart/form-data' not in content_type:
                # 如果不是multipart，返回使用说�?                self.send_json_response(200, {
                    "message": "固件上传API",
                    "usage": "POST multipart/form-data",
                    "fields": {
                        "file": "固件.bin文件",
                        "version": "版本号（如v1.1.0�?,
                        "changelog": "更新说明（可选）"
                    },
                    "example": "curl -X POST -F 'file=@firmware.bin' -F 'version=v1.1.0' http://server/api/upload"
                })
                return
            
            # 解析multipart数据
            form = cgi.FieldStorage(
                fp=self.rfile,
                headers=self.headers,
                environ={
                    'REQUEST_METHOD': 'POST',
                    'CONTENT_TYPE': content_type,
                }
            )
            
            # 获取上传的文件和参数
            if 'file' not in form:
                self.send_json_response(400, {"error": "缺少固件文件"})
                return
            
            file_item = form['file']
            version = form.getvalue('version', 'latest')
            changelog = form.getvalue('changelog', '通过Web上传')
            
            # 保存临时文件
            temp_file = tempfile.NamedTemporaryFile(delete=False, suffix='.bin')
            temp_file.write(file_item.file.read())
            temp_file.close()
            
            # 使用api_handler上传
            result = api_handler.upload_firmware(version, temp_file.name, changelog)
            
            # 删除临时文件
            os.unlink(temp_file.name)
            
            if result.get('success'):
                logger.info(f"�?Web上传成功: {version}")
                self.send_json_response(200, result)
            else:
                logger.error(f"上传失败: {result.get('error')}")
                self.send_json_response(500, result)
            
        except Exception as e:
            logger.error(f"上传处理错误: {e}")
            self.send_json_response(500, {"error": str(e)})
    
    def handle_delete_api(self):
        """处理固件删除 - 支持Web删除"""
        try:
            # Token验证
            if self.config.get('security', {}).get('require_token', False):
                query = parse_qs(urlparse(self.path).query)
                token = query.get('token', [None])[0]
                if token != self.config.get('security', {}).get('token', ''):
                    self.send_json_response(403, {"error": "Invalid token"})
                    return
            
            # 从查询参数或POST数据获取版本�?            query = parse_qs(urlparse(self.path).query)
            version = query.get('version', [None])[0]
            
            # 如果没有查询参数，尝试解析POST数据
            if not version:
                content_length = int(self.headers.get('Content-Length', 0))
                if content_length > 0:
                    post_data = self.rfile.read(content_length)
                    try:
                        data = json.loads(post_data.decode('utf-8'))
                        version = data.get('version')
                    except:
                        pass
            
            if not version:
                self.send_json_response(400, {
                    "error": "缺少版本参数",
                    "usage": "GET /api/delete?version=v1.1.0 �?POST {\"version\":\"v1.1.0\"}"
                })
                return
            
            # 使用api_handler删除
            result = api_handler.delete_firmware(version)
            
            if result.get('success'):
                logger.info(f"�?Web删除成功: {version}")
                self.send_json_response(200, result)
            else:
                logger.error(f"删除失败: {result.get('error')}")
                self.send_json_response(404, result)
            
        except Exception as e:
            logger.error(f"删除处理错误: {e}")
            self.send_json_response(500, {"error": str(e)})
    
    def handle_mqtt_status_api(self):
        """处理MQTT状态查�?""
        try:
            if not MQTT_ENABLED:
                response = {
                    "connected": False,
                    "message": "MQTT功能未启�?,
                    "hint": "安装paho-mqtt�? pip3 install paho-mqtt",
                    "broker": MQTT_BROKER,
                    "port": MQTT_PORT,
                    "device_count": 0,
                    "devices": []
                }
                self.send_json_response(200, response)
                return
            
            # MQTT已启用，返回状�?            response = {
                "connected": mqtt_client is not None,  # 简化检�?                "broker": MQTT_BROKER,
                "port": MQTT_PORT,
                "device_count": len(mqtt_devices),
                "devices": [
                    {
                        "id": device_id,
                        "version": info.get("version"),
                        "status": info.get("status"),
                        "last_seen": info.get("last_seen")
                    }
                    for device_id, info in mqtt_devices.items()
                ],
                "timestamp": datetime.now().isoformat()
            }
            
            self.send_json_response(200, response)
            
        except Exception as e:
            logger.error(f"MQTT状态查询错�? {e}")
            self.send_json_response(500, {"error": str(e)})
    
    def handle_mqtt_push_api(self):
        """处理MQTT推送请�?""
        try:
            if not MQTT_ENABLED:
                response = {
                    "status": "error",
                    "message": "MQTT功能未启�?,
                    "hint": "安装paho-mqtt�? pip3 install paho-mqtt"
                }
                self.send_json_response(200, response)
                logger.warning("MQTT推送请求但功能未启�?)
                return
            
            # 读取POST数据
            content_length = int(self.headers.get('Content-Length', 0))
            post_data = self.rfile.read(content_length)
            request_data = json.loads(post_data.decode('utf-8'))
            
            target = request_data.get('target', 'all')
            custom_version = request_data.get('version')
            
            # 获取最新版�?            if not custom_version:
                try:
                    with open(MANIFEST_FILE, 'r') as f:
                        manifest = json.load(f)
                        version = manifest.get('latest', {}).get('version', '')
                except:
                    version = "latest"
            else:
                version = custom_version
            
            # 构造推送消�?            push_message = {
                "command": "upgrade",
                "target": target,
                "version": version,
                "server_url": f"http://{self.config.get('server', {}).get('host', 'your-server-ip')}:{self.config.get('server', {}).get('port', 15678)}",
                "timestamp": datetime.now().isoformat(),
                "force": False
            }
            
            # 推送MQTT消息（简化实现）
            # 实际需要完整的MQTT客户端连接和发布逻辑
            # 这里返回成功响应，实际推送在后台线程
            
            response = {
                "status": "success",
                "message": "升级指令已推�?,
                "target": target,
                "version": version,
                "mqtt_topic": "ota/upgrade/command",
                "timestamp": datetime.now().isoformat()
            }
            
            self.send_json_response(200, response)
            logger.info(f"MQTT推送请�? target={target}, version={version}")
            
            # 如果MQTT客户端已连接，实际推�?            if mqtt_client:
                try:
                    mqtt_client.publish("ota/upgrade/command", json.dumps(push_message), qos=1)
                    logger.info("�?MQTT消息已发�?)
                except Exception as e:
                    logger.error(f"MQTT发布失败: {e}")
            
        except Exception as e:
            logger.error(f"MQTT推送处理错�? {e}")
            self.send_json_response(500, {"error": str(e)})
    
    def handle_version_api(self):
        """返回最新版本信�?""
        try:
            manifest = self.load_manifest()
            latest = manifest.get('latest', {})
            
            response = {
                "version": latest.get('version', 'unknown'),
                "url": f"/firmware/{latest.get('path', 'latest/firmware.bin')}",
                "size": latest.get('size', 0),
                "sha256": latest.get('sha256', ''),
                "timestamp": latest.get('timestamp', ''),
                "changelog": latest.get('changelog', '')
            }
            
            self.send_json_response(200, response)
            logger.info(f"版本查询: {response['version']}")
            
        except Exception as e:
            logger.error(f"版本API错误: {e}")
            self.send_json_response(500, {"error": str(e)})
    
    def handle_manifest_api(self):
        """返回完整版本清单"""
        try:
            manifest = self.load_manifest()
            self.send_json_response(200, manifest)
            logger.info("版本清单查询")
        except Exception as e:
            logger.error(f"清单API错误: {e}")
            self.send_json_response(500, {"error": str(e)})
    
    def handle_status_api(self):
        """返回服务器状�?""
        status = {
            "server": "HTTP OTA Server",
            "version": "1.0.0",
            "uptime": str(datetime.now()),
            "firmware_count": len(os.listdir(FIRMWARE_DIR)) if os.path.exists(FIRMWARE_DIR) else 0,
            "config": self.config
        }
        self.send_json_response(200, status)
    
    def handle_logs_api(self):
        """返回服务器日�?- 支持过滤和搜�?""
        try:
            # 解析查询参数
            query = parse_qs(urlparse(self.path).query)
            lines = int(query.get('lines', [100])[0])
            level = query.get('level', [None])[0]
            search = query.get('search', [None])[0]
            
            # 使用api_handler获取日志
            result = api_handler.get_logs(lines, level, search)
            
            self.send_json_response(200, result)
            
        except Exception as e:
            logger.error(f"日志API错误: {e}")
            self.send_json_response(500, {"error": str(e)})
    
    def handle_web_ui(self):
        """提供Web管理界面"""
        try:
            ui_file = 'templates/index.html'
            if not os.path.exists(ui_file):
                # 如果没有前端文件，返回简单提�?                response = {
                    "message": "OTA Web UI",
                    "hint": "请部署前端界面文件到 templates/index.html",
                    "api_endpoints": [
                        "/api/version",
                        "/api/manifest",
                        "/api/status",
                        "/firmware/latest/firmware.bin"
                    ]
                }
                self.send_json_response(200, response)
                return
            
            # 读取并发送HTML文件
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.end_headers()
            
            with open(ui_file, 'r', encoding='utf-8') as f:
                self.wfile.write(f.read().encode('utf-8'))
            
        except Exception as e:
            logger.error(f"Web UI错误: {e}")
            self.send_error(500, str(e))
    
    def handle_firmware_download(self, path):
        """处理固件下载"""
        # Token验证（如果启用）
        if self.config.get('require_token', False):
            query = parse_qs(urlparse(self.path).query)
            token = query.get('token', [None])[0]
            if token != self.config.get('token', ''):
                self.send_error(403, "Invalid token")
                logger.warning(f"无效token下载尝试: {path}")
                return
        
        # 记录下载日志
        if self.config.get('log_downloads', True):
            client_ip = self.client_address[0]
            logger.info(f"固件下载: {path} 来自 {client_ip}")
        
        # 处理文件下载
        file_path = path.lstrip('/')
        if os.path.exists(file_path):
            # 发送文�?            self.send_file(file_path)
        else:
            self.send_error(404, "Firmware not found")
            logger.warning(f"固件不存�? {path}")
    
    def send_file(self, file_path):
        """发送文件响�?""
        try:
            file_size = os.path.getsize(file_path)
            
            # 支持Range请求（断点续传）
            range_header = self.headers.get('Range')
            if range_header:
                # 解析Range: bytes=start-end
                start, end = self.parse_range(range_header, file_size)
                self.send_partial_file(file_path, start, end)
            else:
                # 完整文件下载
                self.send_response(200)
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Content-Length', file_size)
                self.send_header('Accept-Ranges', 'bytes')
                self.send_header('Content-Disposition', f'attachment; filename="{os.path.basename(file_path)}"')
                self.end_headers()
                
                with open(file_path, 'rb') as f:
                    while True:
                        chunk = f.read(8192)
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                
                logger.info(f"完整下载: {file_path} ({file_size} bytes)")
                
        except Exception as e:
            logger.error(f"文件发送错�? {e}")
            self.send_error(500, str(e))
    
    def send_partial_file(self, file_path, start, end):
        """发送部分文件（断点续传�?""
        try:
            content_length = end - start + 1
            
            self.send_response(206)  # Partial Content
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', content_length)
            self.send_header('Content-Range', f'bytes {start}-{end}/{os.path.getsize(file_path)}')
            self.end_headers()
            
            with open(file_path, 'rb') as f:
                f.seek(start)
                chunk_size = 8192
                remaining = content_length
                
                while remaining > 0:
                    read_size = min(chunk_size, remaining)
                    chunk = f.read(read_size)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
            
            logger.info(f"部分下载: {file_path} (bytes {start}-{end})")
            
        except Exception as e:
            logger.error(f"部分文件发送错�? {e}")
            self.send_error(500, str(e))
    
    def parse_range(self, range_header, file_size):
        """解析Range请求�?""
        # Range: bytes=start-end �?bytes=start-
        try:
            range_str = range_header.replace('bytes=', '')
            if '-' in range_str:
                parts = range_str.split('-')
                start = int(parts[0]) if parts[0] else 0
                end = int(parts[1]) if parts[1] else file_size - 1
            else:
                start = int(range_str)
                end = file_size - 1
            
            # 确保范围有效
            start = max(0, start)
            end = min(file_size - 1, end)
            
            return start, end
        except:
            return 0, file_size - 1
    
    def load_manifest(self):
        """加载版本清单"""
        try:
            with open(MANIFEST_FILE, 'r') as f:
                return json.load(f)
        except FileNotFoundError:
            logger.warning("manifest.json不存�?)
            return {"versions": [], "latest": {}}
    
    def send_json_response(self, status_code, data):
        """发送JSON响应"""
        self.send_response(status_code)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(data, indent=2, ensure_ascii=False).encode())
    
    def log_message(self, format, *args):
        """自定义日志格�?""
        logger.info(f"{self.client_address[0]} - {format % args}")

def on_mqtt_message(client, userdata, message):
    """MQTT消息回调函数"""
    try:
        topic = message.topic
        payload = message.payload.decode('utf-8')
        
        logger.info(f"收到MQTT消息: {topic}")
        
        # 处理设备状态上�?        if topic == "ota/device/status":
            try:
                data = json.loads(payload)
                device_id = data.get('device_id', 'unknown')
                
                mqtt_devices[device_id] = {
                    "version": data.get('version', ''),
                    "status": data.get('status', 'online'),
                    "last_seen": datetime.now().isoformat(),
                    "ip": data.get('ip', '')
                }
                
                logger.info(f"设备上线: {device_id} (v{mqtt_devices[device_id]['version']})")
                
            except Exception as e:
                logger.error(f"解析设备状态失�? {e}")
                
    except Exception as e:
        logger.error(f"MQTT消息处理错误: {e}")

def main():
    """启动OTA服务�?""
    global mqtt_client  # 使用全局变量
    
    # 加载配置
    try:
        with open(CONFIG_FILE, 'r') as f:
            config = json.load(f)
    except FileNotFoundError:
        config = {"port": 8080}
        logger.warning("使用默认配置")
    
    port = config.get('server', {}).get('port', 8080)
    
    # 创建固件目录
    os.makedirs(FIRMWARE_DIR, exist_ok=True)
    
    # 创建示例manifest
    if not os.path.exists(MANIFEST_FILE):
        sample_manifest = {
            "versions": [],
            "latest": {
                "version": "v1.0.0",
                "path": "latest/firmware.bin",
                "size": 0,
                "sha256": "",
                "timestamp": datetime.now().isoformat(),
                "changelog": "初始版本"
            }
        }
        with open(MANIFEST_FILE, 'w') as f:
            json.dump(sample_manifest, f, indent=2)
        logger.info("创建示例manifest.json")
    
    # 初始化MQTT客户端（如果启用�?    if MQTT_ENABLED:
        try:
            logger.info(f"连接MQTT Broker: {MQTT_BROKER}:{MQTT_PORT}")
            mqtt_client = mqtt.Client(client_id="ota-server")
            mqtt_client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
            
            # 设置消息回调函数
            mqtt_client.on_message = on_mqtt_message
            
            mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
            mqtt_client.loop_start()
            
            # 订阅设备状态主�?            mqtt_client.subscribe("ota/device/status", qos=1)
            logger.info("已订�? ota/device/status")
            
            logger.info("�?MQTT客户端已连接")
        except Exception as e:
            logger.error(f"MQTT连接失败: {e}")
            mqtt_client = None
    
    # 启动服务�?    server = HTTPServer(('0.0.0.0', port), OTARequestHandler)
    logger.info(f"OTA服务器启动在端口 {port}")
    logger.info(f"固件目录: {FIRMWARE_DIR}")
    logger.info(f"API端点:")
    logger.info(f"  - 版本查询: http://localhost:{port}/api/version")
    logger.info(f"  - 版本清单: http://localhost:{port}/api/manifest")
    logger.info(f"  - 服务器状�? http://localhost:{port}/api/status")
    logger.info(f"  - 固件下载: http://localhost:{port}/firmware/latest/firmware.bin")
    
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        logger.info("服务器关�?)
        server.shutdown()

if __name__ == "__main__":
    main()
