/*
 * ============================================================
 *  ESP32 Finger Exoskeleton — main.ino  (Cloud version, 2 ช่อง)
 *  เชื่อมต่อ WebSocket Server บน Railway โดยตรง
 *
 *  ฮาร์ดแวร์: Servo×2, FSR×2, Flex×2, Mic×1
 *
 *  Library ที่ต้องติดตั้งเพิ่ม:
 *   - WebSockets        (by Markus Sattler / Links2004)
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
const char* WS_HOST = "exoskeleton-server-production.up.railway.app"; // ← Railway domain
const int   WS_PORT = 443;                          // Railway ใช้ HTTPS/WSS
const char* WS_PATH = "/esp32";                    // endpoint สำหรับ ESP32

// ── Pin Config (ทั้งหมดเป็น ADC1 ใช้ร่วมกับ WiFi ได้) ────────
// ⚠️ แก้ให้ตรงกับการต่อสายจริงของคุณ
#define NUM_CH 2
const int PIN_FSR[NUM_CH]   = { 34, 35 };  // FSR ช่อง 1, 2  (ADC1)
const int PIN_FLEX[NUM_CH]  = { 32, 33 };  // Flex ช่อง 1, 2 (ADC1)
const int PIN_SERVO[NUM_CH] = { 18, 19 };  // Servo ช่อง 1, 2 (PWM)
#define PIN_MIC    36   // Microphone (ADC1, input-only)
#define PIN_LED     2   // Status LED

// ── Objects ──────────────────────────────────────────────────
WebSocketsClient wsClient;
Servo            myServo[NUM_CH];

// ── State (ต่อช่อง) ──────────────────────────────────────────
int           servoAngle[NUM_CH]        = { 0, 0 };
bool          servoLocked[NUM_CH]       = { false, false };
unsigned long servoMoveStart[NUM_CH]    = { 0, 0 };
bool          servoDetachPending[NUM_CH] = { false, false };

bool          wsConnected    = false;
unsigned long lastSensorSend = 0;

const int SENSOR_INTERVAL  = 100;   // ส่งทุก 100ms
const int SERVO_DETACH_MS  = 600;
const int WIFI_TIMEOUT_MS  = 20000;

// ── Sensor Read ──────────────────────────────────────────────
float readFSRNewton(int ch)   { return analogRead(PIN_FSR[ch])  * (50.0 / 4095.0); }
int   readFlexDegrees(int ch) { return map(analogRead(PIN_FLEX[ch]), 1000, 3500, 0, 90); }
int   readMicRaw()            { return analogRead(PIN_MIC); }

// ── Servo Control (non-blocking) ต่อช่อง ─────────────────────
void moveServo(int ch, int angle, bool lock) {
  if (ch < 0 || ch >= NUM_CH) return;
  angle = constrain(angle, 0, 180);
  if (!myServo[ch].attached()) myServo[ch].attach(PIN_SERVO[ch]);
  myServo[ch].write(angle);
  servoAngle[ch]  = angle;
  servoLocked[ch] = lock;

  if (!lock) {
    servoMoveStart[ch]     = millis();
    servoDetachPending[ch] = true;
  } else {
    servoDetachPending[ch] = false;
  }
  Serial.printf("[SERVO] ch=%d angle=%d lock=%d\n", ch, angle, lock);
}

// สั่งทุกช่อง (เมื่อ command ไม่ระบุ ch)
void moveAll(int angle, bool lock) {
  for (int ch = 0; ch < NUM_CH; ch++) moveServo(ch, angle, lock);
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
    // ถ้าไม่ระบุ ch → -1 = สั่งทุกช่อง
    int         ch        = doc.containsKey("ch") ? (int)doc["ch"] : -1;

    Serial.printf("[CMD] action=%s ch=%d intensity=%d\n", action, ch, intensity);

    if (strcmp(action, "grip") == 0) {
      int a = map(intensity, 0, 100, 0, 180);
      (ch < 0) ? moveAll(a, true) : moveServo(ch, a, true);
    } else if (strcmp(action, "open") == 0) {
      (ch < 0) ? moveAll(0, false) : moveServo(ch, 0, false);
    } else if (strcmp(action, "close") == 0) {
      (ch < 0) ? moveAll(180, true) : moveServo(ch, 180, true);
    } else if (strcmp(action, "angle") == 0) {
      int  angle = doc["angle"] | 90;
      bool lock  = doc["lock"]  | true;
      (ch < 0) ? moveAll(angle, lock) : moveServo(ch, angle, lock);
    }
  }

  // Voice command (สั่งทุกช่องพร้อมกัน)
  else if (strcmp(type, "voice") == 0) {
    String text = doc["text"] | "";
    Serial.println("[VOICE] " + text);

    if (text.indexOf("เปิด")  >= 0) moveAll(0,   false);
    if (text.indexOf("ปิด")   >= 0 || text.indexOf("กำ") >= 0) moveAll(180, true);
    if (text.indexOf("กลาง")  >= 0) moveAll(90,  true);
    if (text.indexOf("ล็อก")  >= 0) for (int ch = 0; ch < NUM_CH; ch++) servoLocked[ch] = true;
    if (text.indexOf("ปล่อย") >= 0) for (int ch = 0; ch < NUM_CH; ch++) moveServo(ch, servoAngle[ch], false);
  }
}

// ── ส่ง Sensor Data ขึ้น Server (รูปแบบ array 2 ช่อง) ────────
void sendSensorData() {
  if (!wsConnected) return;

  StaticJsonDocument<320> doc;
  doc["type"] = "sensor_data";

  JsonArray fsr   = doc.createNestedArray("fsr");
  JsonArray flex  = doc.createNestedArray("flex");
  JsonArray servo = doc.createNestedArray("servo");
  JsonArray lock  = doc.createNestedArray("lock");
  for (int ch = 0; ch < NUM_CH; ch++) {
    fsr.add(readFSRNewton(ch));
    flex.add(readFlexDegrees(ch));
    servo.add(servoAngle[ch]);
    lock.add(servoLocked[ch]);
  }

  doc["mic"]       = readMicRaw();
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
      wsClient.sendTXT("{\"type\":\"hello\",\"device\":\"ESP32-Exoskeleton\",\"version\":\"2.0\"}");
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

  // Servo init (ทุกช่อง)
  for (int ch = 0; ch < NUM_CH; ch++) {
    myServo[ch].attach(PIN_SERVO[ch]);
    myServo[ch].write(0);
  }
  delay(500);
  for (int ch = 0; ch < NUM_CH; ch++) myServo[ch].detach();

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
  wsClient.setReconnectInterval(3000);       // reconnect ทุก 3 วิถ้าหลุด
  wsClient.enableHeartbeat(15000, 3000, 2);  // ping ทุก 15 วิ

  Serial.println("[WS] Connecting to server...");
}

// ── Loop ─────────────────────────────────────────────────────
void loop() {
  wsClient.loop();   // ต้องเรียกทุก loop — จัดการ reconnect ให้อัตโนมัติ

  // Servo detach (non-blocking) ต่อช่อง
  for (int ch = 0; ch < NUM_CH; ch++) {
    if (servoDetachPending[ch] && millis() - servoMoveStart[ch] >= SERVO_DETACH_MS) {
      myServo[ch].detach();
      servoDetachPending[ch] = false;
    }
  }

  // ส่ง sensor ทุก 100ms
  if (millis() - lastSensorSend >= SENSOR_INTERVAL) {
    sendSensorData();
    lastSensorSend = millis();
  }
}
