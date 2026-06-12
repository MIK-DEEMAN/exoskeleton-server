/*
 * ============================================================
 *  EMF Voltage Monitor — ESP32 Cloud Edition
 *  Hardware : ESP32 + ADS1115
 *  Libraries: Adafruit ADS1X15, ArduinoWebsockets, ArduinoJson
 *
 *  แก้ไข 3 บรรทัดนี้ก่อน Upload:
 *    ssid       → ชื่อ WiFi
 *    password   → รหัส WiFi
 *    SERVER_HOST → URL ของ Railway server (ไม่มี wss:// ไม่มี /)
 * ============================================================
 */

#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <ArduinoWebsockets.h>   // โดย Gil Maimon
#include <ArduinoJson.h>

// ── Config ─────────────────────────────────────────────────────
const char* ssid        = "YOUR_SSID";
const char* password    = "YOUR_PASSWORD";
const char* SERVER_HOST = "YOUR_APP.railway.app";  // ← เปลี่ยนตรงนี้
const int   SERVER_PORT = 443;
const char* SERVER_PATH = "/esp32";

const int   SEND_INTERVAL_MS = 100;  // ส่งทุก 100ms = 10 Hz

// ── Objects ───────────────────────────────────────────────────
Adafruit_ADS1115           ads;
websockets::WebsocketsClient ws;

// ── Measurement State ─────────────────────────────────────────
float voltage_mV      = 0;
float min_mV          = 99999.f;
float max_mV          = -99999.f;
float accumulated_mV  = 0;
float average_mV      = 0;
unsigned long sample_count = 0;
unsigned long last_send    = 0;
unsigned long last_connect = 0;
bool wsConnected = false;

// ── Reconnect helper ──────────────────────────────────────────
void connectWS() {
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.print("[WS] Connecting to server...");
  wsConnected = ws.connectSSL(SERVER_HOST, SERVER_PORT, SERVER_PATH);
  if (wsConnected) Serial.println(" OK");
  else             Serial.println(" FAILED (retry in 5s)");
}

// ── Incoming message handler (reset command) ──────────────────
void onMessage(websockets::WebsocketsMessage msg) {
  StaticJsonDocument<64> doc;
  if (deserializeJson(doc, msg.data()) != DeserializationError::Ok) return;

  if (strcmp(doc["type"], "reset") == 0) {
    min_mV         = 99999.f;
    max_mV         = -99999.f;
    accumulated_mV = 0;
    average_mV     = 0;
    sample_count   = 0;
    Serial.println("[CMD] Stats reset");
  }
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // ADS1115 — gain ±256 mV (1 bit = 0.0078125 mV)
  ads.setGain(GAIN_SIXTEEN);
  if (!ads.begin()) {
    Serial.println("[ERR] ADS1115 not found!");
    while (1) delay(1000);
  }
  Serial.println("[HW] ADS1115 OK");

  WiFi.begin(ssid, password);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\n[WiFi] IP: " + WiFi.localIP().toString());

  ws.onMessage(onMessage);
  ws.onEvent([](websockets::WebsocketsEvent event, String data) {
    if (event == websockets::WebsocketsEvent::ConnectionOpened)  { wsConnected = true;  Serial.println("[WS] Connected"); }
    if (event == websockets::WebsocketsEvent::ConnectionClosed)  { wsConnected = false; Serial.println("[WS] Disconnected"); }
  });

  connectWS();
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {
  // Auto-reconnect
  if (!wsConnected && millis() - last_connect > 5000) {
    last_connect = millis();
    connectWS();
  }

  ws.poll();

  // Read ADS1115 differential CH0–CH1
  int16_t raw = ads.readADC_Differential_0_1();
  voltage_mV  = raw * 0.0078125f;

  // Update stats
  if (voltage_mV < min_mV) min_mV = voltage_mV;
  if (voltage_mV > max_mV) max_mV = voltage_mV;

  float absV = fabsf(voltage_mV);
  accumulated_mV += absV;
  sample_count++;
  average_mV = accumulated_mV / (float)sample_count;

  // Send at SEND_INTERVAL_MS
  if (wsConnected && millis() - last_send >= SEND_INTERVAL_MS) {
    StaticJsonDocument<200> doc;
    doc["type"] = "sensor_data";
    doc["cur"]  = voltage_mV;
    doc["min"]  = min_mV;
    doc["max"]  = max_mV;
    doc["avg"]  = average_mV;
    doc["acc"]  = accumulated_mV;

    char buf[200];
    serializeJson(doc, buf);
    ws.send(buf);
    last_send = millis();
  }
}
