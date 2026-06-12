/*
 * ============================================================
 *  ESP32 Finger Exoskeleton — main.ino  (Cloud version)
 *  เชื่อมต่อ WebSocket Server บน Railway โดยตรง
 *
 *  Library ที่ต้องติดตั้งเพิ่ม:
 *   - WebSocketsClient  (by Markus Sattler)
 *   - ArduinoJson       (by Benoit Blanchon)
 *   - ESP32Servo        (by Kevin Harrington)
 * ============================================================
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

// ── WiFi Config ─────────────────────────────────────────────
const char* WIFI_SSID     = "YOUR_HOTSPOT_NAME";   // ← แก้
const char* WIFI_PASSWORD = "YOUR_HOTSPOT_PASS";   // ← แก้

// ── Railway Server Config ────────────────────────────────────
// ได้ URL หลัง deploy Railway เช่น: your-app.railway.app
const char* WS_HOST = "your-app.railway.app";      // ← แก้หลัง deploy
const int   WS_PORT = 443;                          // Railway ใช้ HTTPS/WSS
const char* WS_PATH = "/esp32";                    // endpoint สำหรับ ESP32

// ── Pin Config ───────────────────────────────────────────────
#define PIN_FSR    34   // FSR Sensor  (ADC1)
#define PIN_FLEX   35   // Flex Sensor (ADC1)
#define PIN_MIC    32   // Microphone  (ADC1)
#define PIN_SERVO  18   // Servo PWM
#define PIN_LED     2   // Status LED

// ── Objects ──────────────────────────────────────────────────
WebSocketsClient wsClient;
Servo            myServo;

// ── State ────────────────────────────────────────────────────
int           servoAngle        = 0;
bool          servoLocked       = false;
bool          wsConnected       = false;
unsigned long lastSensorSend    = 0;
unsigned long servoMoveStart    = 0;
bool          servoDetachPending = false;

const int SENSOR_INTERVAL  = 100;   // ส่งทุก 100ms
const int SERVO_DETACH_MS  = 600;
const int WIFI_TIMEOUT_MS  = 20000;

// ── Sensor Read ──────────────────────────────────────────────
float readFSRNewton()   { return analogRead(PIN_FSR)  * (50.0 / 4095.0); }
int   readFlexDegrees() { return map(analogRead(PIN_FLEX), 1000, 3500, 0, 90); }
int   readMicRaw()      { return analogRead(PIN_MIC); }

// ── Servo Control (non-blocking) ─────────────────────────────
void moveServo(int angle, bool lock) {
  angle = constrain(angle, 0, 180);
  if (!myServo.attached()) myServo.attach(PIN_SERVO);
  myServo.write(angle);
  servoAngle  = angle;
  servoLocked = lock;

  if (!lock) {
    servoMoveStart    = millis();
    servoDetachPending = true;
  } else {
    servoDetachPending = false;
  }
  Serial.printf("[SERVO] angle=%d lock=%d\n", angle, lock);
}

// ── Parse & Handle Command จาก Server ───────────────────────
void handleCommand(const String& msg) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, msg) != DeserializationError::Ok) return;

  const char* type = doc["type"];

  // Manual motor command
  if (strcmp(type, "command") == 0) {
    const char* action    = doc["action"]    | "none";
    int         intensity = doc["intensity"] | 50;

    Serial.printf("[CMD] action=%s intensity=%d\n", action, intensity);

    if (strcmp(action, "grip") == 0) {
      moveServo(map(intensity, 0, 100, 0, 180), true);
    } else if (strcmp(action, "open") == 0) {
      moveServo(0, false);
    } else if (strcmp(action, "close") == 0) {
      moveServo(180, true);
    } else if (strcmp(action, "angle") == 0) {
      int angle = doc["angle"] | 90;
      moveServo(angle, doc["lock"] | true);
    }
  }

  // Voice command
  else if (strcmp(type, "voice") == 0) {
    String text = doc["text"] | "";
    Serial.println("[VOICE] " + text);

    // แปลงคำสั่งเสียงภาษาไทย
    if (text.indexOf("เปิด")   >= 0 || text.indexOf("펴") >= 0) moveServo(0,   false);
    if (text.indexOf("ปิด")    >= 0 || text.indexOf("กำ")>= 0) moveServo(180, true);
    if (text.indexOf("กลาง")   >= 0)                            moveServo(90,  true);
    if (text.indexOf("ล็อก")   >= 0)                            servoLocked = true;
    if (text.indexOf("ปล่อย")  >= 0) { moveServo(servoAngle, false); }
  }
}

// ── ส่ง Sensor Data ขึ้น Server ─────────────────────────────
void sendSensorData() {
  if (!wsConnected) return;

  StaticJsonDocument<200> doc;
  doc["type"]      = "sensor_data";
  doc["fsr"]       = readFSRNewton();
  doc["flex"]      = readFlexDegrees();
  doc["mic"]       = readMicRaw();
  doc["servo"]     = servoAngle;
  doc["lock"]      = servoLocked;
  doc["timestamp"] = millis();

  String json;
  serializeJson(doc, json);
  wsClient.sendTXT(json);
}

// ── WebSocket Event Callback ──────────────────────────────────
void onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      wsConnected = false;
      digitalWrite(PIN_LED, LOW);
      Serial.println("[WS] Disconnected — จะ reconnect อัตโนมัติ");
      break;

    case WStype_CONNECTED:
      wsConnected = true;
      digitalWrite(PIN_LED, HIGH);
      Serial.printf("[WS] Connected to %s%s\n", WS_HOST, WS_PATH);
      // ส่ง hello เพื่อแจ้งว่าเป็น ESP32
      wsClient.sendTXT("{\"type\":\"hello\",\"device\":\"ESP32-Exoskeleton\",\"version\":\"1.0\"}");
      break;

    case WStype_TEXT:
      {
        String msg = String((char*)payload).substring(0, length);
        Serial.println("[WS] Received: " + msg);
        handleCommand(msg);
      }
      break;

    case WStype_ERROR:
      Serial.println("[WS] Error!");
      break;

    case WStype_PING:
      Serial.println("[WS] Ping received");
      break;

    default:
      break;
  }
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);

  // Servo init
  myServo.attach(PIN_SERVO);
  myServo.write(0);
  delay(500);
  myServo.detach();

  // WiFi — มี timeout
  Serial.print("[WiFi] Connecting to " + String(WIFI_SSID));
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > WIFI_TIMEOUT_MS) {
      Serial.println("\n[WiFi] Timeout! รีสตาร์ท...");
      delay(3000);
      ESP.restart();
    }
    Serial.print(".");
    delay(500);
  }
  Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());

  // WebSocket Client — เชื่อมต่อ Railway (WSS = SSL)
  wsClient.beginSSL(WS_HOST, WS_PORT, WS_PATH);
  wsClient.onEvent(onWebSocketEvent);
  wsClient.setReconnectInterval(3000);   // reconnect ทุก 3 วิถ้าหลุด
  wsClient.enableHeartbeat(15000, 3000, 2); // ping ทุก 15 วิ

  Serial.println("[WS] Connecting to server...");
}

// ── Loop ─────────────────────────────────────────────────────
void loop() {
  wsClient.loop();   // ต้องเรียกทุก loop — จัดการ reconnect ให้อัตโนมัติ

  // Servo detach (non-blocking)
  if (servoDetachPending && millis() - servoMoveStart >= SERVO_DETACH_MS) {
    myServo.detach();
    servoDetachPending = false;
  }

  // ส่ง sensor ทุก 100ms
  if (millis() - lastSensorSend >= SENSOR_INTERVAL) {
    sendSensorData();
    lastSensorSend = millis();
  }
}
