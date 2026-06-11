const express = require('express');
const mqtt = require('mqtt');
const fs = require('fs');
const path = require('path');

const app = express();
const PORT = 18597;

// MQTT Settings
const MQTT_BROKER = 'mqtt://127.0.0.1:1883';
const MQTT_USER = process.env.MQTT_USER || 'YOUR_MQTT_USERNAME';
const MQTT_PASS = process.env.MQTT_PASS || 'YOUR_MQTT_PASSWORD';
const PUB_TOPIC = 'home/gateway/1527map';
const SUB_TOPIC = 'home/gateway/1527map_resp';

// State
let lastResponse = null;
const clients = []; // SSE clients

// Middleware
app.use(express.json());
app.use(express.static(__dirname));

// Connect to MQTT
const mqttClient = mqtt.connect(MQTT_BROKER, {
  username: MQTT_USER,
  password: MQTT_PASS,
  keepalive: 60,
  reconnectPeriod: 3000,
});

mqttClient.on('connect', () => {
  console.log('[MQTT] Connected to broker');
  mqttClient.subscribe(SUB_TOPIC, { qos: 1 });
});

mqttClient.on('message', (topic, message) => {
  try {
    const payload = JSON.parse(message.toString());
    console.log('[MQTT] Received:', payload);
    lastResponse = payload;
    // Notify all SSE clients
    clients.forEach(client => {
      client.write(`data: ${JSON.stringify({ type: 'response', data: payload })}\n\n`);
    });
  } catch (e) {
    console.error('[MQTT] Parse error:', e.message);
  }
});

mqttClient.on('error', (err) => {
  console.error('[MQTT] Error:', err.message);
});

mqttClient.on('offline', () => {
  console.log('[MQTT] Offline');
});

// SSE endpoint
app.get('/api/subscribe', (req, res) => {
  res.setHeader('Content-Type', 'text/event-stream');
  res.setHeader('Cache-Control', 'no-cache');
  res.setHeader('Connection', 'keep-alive');
  res.setHeader('X-Accel-Buffering', 'no');
  res.flushHeaders();

  // Send initial connected message
  res.write(`data: ${JSON.stringify({ type: 'connected' })}\n\n`);
  console.log('[SSE] Client connected');

  clients.push(res);

  // Keep-alive ping
  const ping = setInterval(() => {
    res.write(`: ping\n\n`);
  }, 30000);

  req.on('close', () => {
    clearInterval(ping);
    const idx = clients.indexOf(res);
    if (idx > -1) clients.splice(idx, 1);
    console.log('[SSE] Client disconnected');
  });
});

// API: Check status
app.get('/api/status', (req, res) => {
  res.json({
    mqtt_connected: mqttClient.connected,
    last_response: lastResponse
  });
});

// API: Publish message
app.post('/api/send', (req, res) => {
  if (!mqttClient.connected) {
    return res.status(500).json({ error: 'MQTT not connected' });
  }
  const { cmd, data } = req.body;
  const payload = JSON.stringify({ cmd, data });
  mqttClient.publish(PUB_TOPIC, payload, { qos: 1 }, (err) => {
    if (err) {
      console.error('[API] Publish error:', err);
      return res.status(500).json({ error: err.message });
    }
    console.log('[API] Published:', payload);
    res.json({ success: true });
  });
});

// API: Request map from ESP32
app.get('/api/load', (req, res) => {
  if (!mqttClient.connected) {
    return res.status(500).json({ error: 'MQTT not connected' });
  }
  mqttClient.publish(PUB_TOPIC, JSON.stringify({ cmd: 'get_map' }), { qos: 1 });
  res.json({ success: true });
});

// Start server
app.listen(PORT, '0.0.0.0', () => {
  console.log(`[1527 Mapper] Server running on http://0.0.0.0:${PORT}`);
  console.log(`[1527 Mapper] MQTT: ${MQTT_BROKER}`);
  console.log(`[1527 Mapper] Topics: ${PUB_TOPIC} / ${SUB_TOPIC}`);
});
