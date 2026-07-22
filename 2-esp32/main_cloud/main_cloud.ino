/*
 * ============================================================
 *  ESP32 Finger Exoskeleton — main.ino
 *  นิ้วกลไฟฟ้าสั่งการด้วยเสียงสำหรับผู้ป่วยนิ้วมืออ่อนแรง
 *
 *  ฮาร์ดแวร์: ESP32 + Servo MG92B x2 + FSR x2
 *  กลไก: เชือก 2 เส้นต่อ 1 เซอร์โว (ดึงงอ / ดึงเหยียด)
 *
 *  Library ที่ต้องติดตั้งเพิ่ม:
 *   - WebSockets   (Markus Sattler / Links2004)
 *   - ArduinoJson  (Benoit Blanchon)
 *   - ESP32Servo   (Kevin Harrington)
 *
 *  หมายเหตุ: การรู้จำเสียงทำที่เบราว์เซอร์ (Web Speech API)
 *  ESP32 รับเป็นข้อความที่ถอดเสร็จแล้วผ่าน WebSocket
 * ============================================================
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

// ── โหมดสอบเทียบ FSR ────────────────────────────────────────
// ตั้งเป็น 1 เพื่อพิมพ์ค่า ADC ดิบทาง Serial สำหรับทำกราฟสอบเทียบ
// เมื่อสอบเทียบเสร็จแล้วให้ตั้งกลับเป็น 0
#define CALIB_MODE 0

// ── WiFi Config ─────────────────────────────────────────────
const char* WIFI_SSID     = "<<ใส่ชื่อ Hotspot>>";
const char* WIFI_PASSWORD = "<<ใส่รหัสผ่าน>>";

// ── Server Config ───────────────────────────────────────────
const char* WS_HOST = "exoskeleton-server.onrender.com";
const int   WS_PORT = 443;          // HTTPS/WSS
const char* WS_PATH = "/esp32";

// ── Pin Config (ADC1 ใช้ร่วมกับ WiFi ได้) ───────────────────
#define NUM_CH 2
const int PIN_FSR[NUM_CH]   = { 34, 35 };  // FSR นิ้วที่ 1, 2
const int PIN_SERVO[NUM_CH] = { 18, 19 };  // Servo นิ้วที่ 1, 2
#define PIN_LED   2

// ── Objects ─────────────────────────────────────────────────
WebSocketsClient wsClient;
Servo            myServo[NUM_CH];

// ── State (ต่อช่อง) ─────────────────────────────────────────
int           servoTarget[NUM_CH]    = { 0, 0 };
float         servoPos[NUM_CH]       = { 0, 0 };
float         servoStart[NUM_CH]     = { 0, 0 };
bool          servoLocked[NUM_CH]    = { false, false };
unsigned long servoMoveStart[NUM_CH] = { 0, 0 };
float         servoMoveDur[NUM_CH]   = { 0, 0 };
unsigned long servoLastUpd[NUM_CH]   = { 0, 0 };

bool          wsConnected    = false;
unsigned long lastSensorSend = 0;

const int SENSOR_INTERVAL = 100;     // ส่งค่าเซนเซอร์ทุก 100 ms
const int WIFI_TIMEOUT_MS = 20000;

// ── พารามิเตอร์การเคลื่อนที่ของเซอร์โว ──────────────────────
// MG92B: 180° เต็มต้องใช้ช่วงพัลส์ ~500–2400 µs
// ถ้าได้ยินเสียงครางที่ปลายสุด = ชนขอบกลไก ให้หดช่วงเข้า
const float SERVO_SPEED_DPS = 90.0;  // องศา/วินาที
const int   SERVO_UPDATE_MS = 10;    // อัปเดตทุก 10 ms (100 Hz)
const int   SERVO_MIN_US    = 550;   // พัลส์ที่ 0°
const int   SERVO_MAX_US    = 2350;  // พัลส์ที่ 180°

// ── ค่าคงที่สำหรับแปลงค่า FSR ───────────────────────────────
// ⚠️ ค่านี้ต้องมาจากการสอบเทียบด้วยน้ำหนักมาตรฐาน
//    ดูขั้นตอนในภาคผนวก — เปลี่ยนตัวเลขให้ตรงกับผลสอบเทียบจริง
const float FSR_FULL_SCALE_N = 50.0;   // แรง (N) ที่ทำให้ ADC อ่านได้ 0
const int   ADC_MAX          = 4095;

// ── อ่านค่า FSR ─────────────────────────────────────────────
int readFSRRaw(int ch) { return analogRead(PIN_FSR[ch]); }

// แปลง ADC เป็นแรงโดยประมาณ
// หมายเหตุ: เป็นการแปลงเชิงเส้น ค่าที่ได้เป็นค่าประมาณ
// และจะอิ่มตัวเมื่อแรงเกินช่วงการวัดของ FSR
float readFSRNewton(int ch) {
  return (ADC_MAX - readFSRRaw(ch)) * (FSR_FULL_SCALE_N / ADC_MAX);
}

// ── เขียนมุมเป็นความกว้างพัลส์ (ละเอียดกว่า write(int)) ─────
void writeServoMicros(int ch, float deg) {
  if (!myServo[ch].attached()) return;
  float us = SERVO_MIN_US + (deg / 180.0) * (SERVO_MAX_US - SERVO_MIN_US);
  myServo[ch].writeMicroseconds((int)roundf(us));
}

// ── ตั้งเป้าหมายการหมุน (การขยับจริงทำใน stepServos) ────────
// ไม่ detach เพื่อค้างตำแหน่งไว้ กันกลไกดันกลับ
void moveServo(int ch, int angle, bool lock) {
  if (ch < 0 || ch >= NUM_CH) return;
  int target = constrain(angle, 0, 180);
  servoStart[ch]     = servoPos[ch];
  servoTarget[ch]    = target;
  servoMoveStart[ch] = millis();
  servoMoveDur[ch]   = (fabs(target - servoPos[ch]) / SERVO_SPEED_DPS) * 1000.0;
  servoLocked[ch]    = lock;
  if (!myServo[ch].attached()) {
    myServo[ch].attach(PIN_SERVO[ch], SERVO_MIN_US, SERVO_MAX_US);
    writeServoMicros(ch, servoPos[ch]);
  }
  Serial.printf("[SERVO] ch=%d target=%d lock=%d\n", ch, target, lock);
}

void moveAll(int angle, bool lock) {
  for (int ch = 0; ch < NUM_CH; ch++) moveServo(ch, angle, lock);
}

// ── ขยับเซอร์โวแบบ ease-in/out ──────────────────────────────
void stepServos() {
  unsigned long now = millis();
  for (int ch = 0; ch < NUM_CH; ch++) {
    if (servoPos[ch] != (float)servoTarget[ch] && now - servoLastUpd[ch] >= SERVO_UPDATE_MS) {
      servoLastUpd[ch] = now;
      float t = servoMoveDur[ch] > 0 ? (now - servoMoveStart[ch]) / servoMoveDur[ch] : 1.0;
      if (t >= 1.0) {
        servoPos[ch] = servoTarget[ch];
      } else {
        float eased = t * t * (3.0 - 2.0 * t);   // smoothstep
        servoPos[ch] = servoStart[ch] + (servoTarget[ch] - servoStart[ch]) * eased;
      }
      writeServoMicros(ch, servoPos[ch]);
    }
  }
}

// ── รับคำสั่งจาก Server ─────────────────────────────────────
void handleCommand(const String& msg) {
  JsonDocument doc;
  if (deserializeJson(doc, msg) != DeserializationError::Ok) return;

  const char* type = doc["type"];
  if (!type) return;

  // คำสั่งจากปุ่มบนแดชบอร์ด
  if (strcmp(type, "command") == 0) {
    const char* action    = doc["action"]    | "none";
    int         intensity = doc["intensity"] | 50;
    int         ch        = doc["ch"].is<int>() ? (int)doc["ch"] : -1;   // -1 = ทุกช่อง

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

  // คำสั่งเสียง — ข้อความถอดเสียงมาจากเบราว์เซอร์แล้ว
  // "จับ" = ดึงเชือกเส้นงอ (0°) ค้างแรงไว้
  // "ปล่อย" = ดึงเชือกเส้นเหยียด (180°) คืนตำแหน่ง
  else if (strcmp(type, "voice") == 0) {
    String text = doc["text"] | "";
    Serial.println("[VOICE] " + text);

    if (text.indexOf("ปล่อย") >= 0)      moveAll(180, false);
    else if (text.indexOf("จับ") >= 0)   moveAll(0, true);
  }
}

// ── ส่งค่าเซนเซอร์ขึ้น Server ───────────────────────────────
void sendSensorData() {
  if (!wsConnected) return;

  JsonDocument doc;
  doc["type"] = "sensor_data";

  JsonArray fsr   = doc["fsr"].to<JsonArray>();
  JsonArray raw   = doc["fsr_raw"].to<JsonArray>();   // ค่า ADC ดิบ ใช้ตอนสอบเทียบ
  JsonArray servo = doc["servo"].to<JsonArray>();
  JsonArray lock  = doc["lock"].to<JsonArray>();

  for (int ch = 0; ch < NUM_CH; ch++) {
    fsr.add(readFSRNewton(ch));
    raw.add(readFSRRaw(ch));
    servo.add((int)roundf(servoPos[ch]));
    lock.add(servoLocked[ch]);
  }

  doc["timestamp"] = millis();

  String json;
  serializeJson(doc, json);
  wsClient.sendTXT(json);
}

// ── WebSocket Events ────────────────────────────────────────
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
      wsClient.sendTXT("{\"type\":\"hello\",\"device\":\"ESP32-Exoskeleton\"}");
      break;

    case WStype_TEXT: {
      String msg = String((char*)payload, length);
      Serial.println("[WS] Received: " + msg);
      handleCommand(msg);
      break;
    }

    case WStype_ERROR:
      Serial.println("[WS] Error!");
      break;

    default:
      break;
  }
}

// ── Setup ───────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);

  // ตั้งเซอร์โวทุกช่องไว้ที่ 0° (ตำแหน่งเหยียด)
  for (int ch = 0; ch < NUM_CH; ch++) {
    myServo[ch].attach(PIN_SERVO[ch], SERVO_MIN_US, SERVO_MAX_US);
    writeServoMicros(ch, 0);
  }
  delay(500);

#if CALIB_MODE
  Serial.println("=== โหมดสอบเทียบ FSR ===");
  Serial.println("วางน้ำหนักมาตรฐานบน FSR แล้วอ่านค่า ADC");
  Serial.println("time_ms,adc_ch0,adc_ch1");
  return;   // ข้ามการต่อ WiFi ตอนสอบเทียบ
#endif

  // WiFi
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

  // WebSocket (WSS)
  wsClient.beginSSL(WS_HOST, WS_PORT, WS_PATH);
  wsClient.onEvent(onWebSocketEvent);
  wsClient.setReconnectInterval(3000);
  wsClient.enableHeartbeat(15000, 3000, 2);

  Serial.println("[WS] Connecting to server...");
}

// ── Loop ────────────────────────────────────────────────────
void loop() {
#if CALIB_MODE
  // พิมพ์ค่า ADC ดิบทุก 500 ms สำหรับจดลงตารางสอบเทียบ
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 500) {
    lastPrint = millis();
    Serial.printf("%lu,%d,%d\n", millis(), readFSRRaw(0), readFSRRaw(1));
  }
  return;
#endif

  wsClient.loop();
  stepServos();

  if (millis() - lastSensorSend >= SENSOR_INTERVAL) {
    sendSensorData();
    lastSensorSend = millis();
  }
}
