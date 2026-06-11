#!/usr/bin/env python3
import os
"""
1527 Mapper - HTTP API Gateway
Relays MQTT messages between browser and ESP32 gateway
"""

import asyncio
import json
import threading
from flask import Flask, request, jsonify, Response, send_file
from gevent.pywsgi import WSGIServer
import paho.mqtt.client as mqtt

app = Flask(__name__)

# MQTT Settings
MQTT_BROKER = os.environ.get("MQTT_BROKER", "your-mqtt-broker.example.com")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_USER = os.environ.get("MQTT_USER", "your-mqtt-username")
MQTT_PASS = os.environ.get("MQTT_PASS", "your-mqtt-password")
MQTT_TOPIC_PUB = "home/gateway/1527map"
MQTT_TOPIC_SUB = "home/gateway/1527map_resp"

# Global state
last_response = {"data": None, "timestamp": None}
response_event = threading.Event()


def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        print(f"[MQTT] Connected to broker")
        client.subscribe(MQTT_TOPIC_SUB, qos=1)
    else:
        print(f"[MQTT] Connection failed with code {rc}")


def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
        print(f"[MQTT] Received on {msg.topic}: {payload}")
        last_response["data"] = payload
        last_response["timestamp"] = asyncio.get_event_loop().time() if asyncio.get_event_loop().is_running() else 0
        response_event.set()
        response_event.clear()
    except Exception as e:
        print(f"[MQTT] Error parsing message: {e}")


def on_disconnect(client, userdata, rc, properties=None):
    print(f"[MQTT] Disconnected, reconnecting...")


# MQTT client setup (singleton)
mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
mqtt_client.username_password_set(MQTT_USER, MQTT_PASS)
mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message
mqtt_client.on_disconnect = on_disconnect
mqtt_client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
mqtt_client.loop_start()


# --- API Routes ---

@app.route("/")
def index():
    return send_file("index.html")


@app.route("/api/status")
def status():
    """Check MQTT connection status"""
    connected = mqtt_client.is_connected()
    return jsonify({
        "mqtt_connected": connected,
        "last_response": last_response["data"]
    })


@app.route("/api/send", methods=["POST"])
def send():
    """Publish a message to MQTT topic"""
    if not mqtt_client.is_connected():
        return jsonify({"error": "MQTT not connected"}), 500

    data = request.get_json()
    if not data:
        return jsonify({"error": "No data provided"}), 400

    payload = json.dumps(data)
    result = mqtt_client.publish(MQTT_TOPIC_PUB, payload, qos=1)
    
    if result.rc == mqtt.MQTT_ERR_SUCCESS:
        print(f"[API] Published to {MQTT_TOPIC_PUB}: {data}")
        return jsonify({"success": True, "message_id": result.mid})
    else:
        return jsonify({"error": f"MQTT error code {result.rc}"}), 500


@app.route("/api/subscribe")
def subscribe():
    """Server-Sent Events endpoint for receiving MQTT responses"""
    def generate():
        # Send initial connection确认
        yield f"data: {json.dumps({'type': 'connected'})}\n\n"
        
        while True:
            response_event.wait(timeout=30)
            if last_response["data"]:
                yield f"data: {json.dumps({'type': 'response', 'data': last_response['data']})}\n\n"
                last_response["data"] = None

    return Response(generate(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache",
                             "Connection": "keep-alive",
                             "X-Accel-Buffering": "no"})


@app.route("/api/load")
def load():
    """Load current map from ESP32"""
    if not mqtt_client.is_connected():
        return jsonify({"error": "MQTT not connected"}), 500
    
    result = mqtt_client.publish(MQTT_TOPIC_PUB, json.dumps({"cmd": "get_map"}), qos=1)
    return jsonify({"success": result.rc == mqtt.MQTT_ERR_SUCCESS})


if __name__ == "__main__":
    import os
    work_dir = os.path.dirname(os.path.abspath(__file__))
    
    print(f"[1527 Mapper] Starting HTTP server on :18597")
    print(f"[1527 Mapper] MQTT broker: {MQTT_BROKER}:{MQTT_PORT}")
    print(f"[1527 Mapper] Publish topic: {MQTT_TOPIC_PUB}")
    print(f"[1527 Mapper] Subscribe topic: {MQTT_TOPIC_SUB}")
    
    http_server = WSGIServer(("0.0.0.0", 18597), app)
    http_server.serve_forever()
