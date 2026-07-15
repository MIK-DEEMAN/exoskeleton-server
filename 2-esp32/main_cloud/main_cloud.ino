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
#include <ESP_I2S.h>   // ESP32 core 3.x — I2S สำหรับไมค์ INMP441

// ── WiFi Config ─────────────────────────────────────────────
const char* WIFI_SSID     = "OPPO A57 (MIK)";   // ← แก้
const char* WIFI_PASSWORD = "yourfriends";   // ← แก้

// ── Server Config (Render free) ──────────────────────────────
const char* WS_HOST = "exoskeleton-server.onrender.com"; // ← Render domain
const int   WS_PORT = 443;                          // Render ใช้ HTTPS/WSS
const char* WS_PATH = "/esp32";                    // endpoint สำหรับ ESP32

// ── Pin Config (ทั้งหมดเป็น ADC1 ใช้ร่วมกับ WiFi ได้) ────────
// ⚠️ แก้ให้ตรงกับการต่อสายจริงของคุณ
#define NUM_CH 2
const int PIN_FSR[NUM_CH]   = { 34, 35 };  // FSR ช่อง 1, 2  (ADC1)
const int PIN_FLEX[NUM_CH]  = { 32, 33 };  // Flex ช่อง 1, 2 (ADC1)
const int PIN_SERVO[NUM_CH] = { 18, 19 };  // Servo ช่อง 1, 2 (PWM)
#define PIN_LED     2   // Status LED

// ── ไมค์ INMP441 (I2S ดิจิทัล) ───────────────────────────────
// ต่อสาย: VDD→3V3, GND→GND, L/R→GND (เลือกแชนเนลซ้าย)
#define MIC_SCK   26    // INMP441 SCK (Bit Clock)
#define MIC_WS    25    // INMP441 WS  (Word Select / LRCLK)
#define MIC_SD    22    // INMP441 SD  (Serial Data → ESP32)
#define MIC_RATE     16000   // sample rate (Hz)
#define MIC_SAMPLES  256     // จำนวน sample ต่อรอบอ่าน (~16ms ที่ 16kHz)
#define MIC_REF      3000.0  // RMS อ้างอิงสำหรับ map → 100% (ปรับตามความไว)

// ── Objects ──────────────────────────────────────────────────
WebSocketsClient wsClient;
Servo            myServo[NUM_CH];
I2SClass         I2S;
int16_t          micBuf[MIC_SAMPLES];
int              micLevel = 0;          // 0–100%

// ── State (ต่อช่อง) ──────────────────────────────────────────
int           servoTarget[NUM_CH]        = { 0, 0 };   // มุมเป้าหมาย
float         servoPos[NUM_CH]           = { 0, 0 };   // มุมปัจจุบัน (ลื่นไหล)
float         servoStart[NUM_CH]         = { 0, 0 };   // มุมตอนเริ่มขยับ
bool          servoLocked[NUM_CH]        = { false, false };
unsigned long servoMoveStart[NUM_CH]     = { 0, 0 };   // เวลาเริ่มขยับ
float         servoMoveDur[NUM_CH]       = { 0, 0 };   // ระยะเวลาขยับทั้งท่อน (ms)
unsigned long servoLastUpd[NUM_CH]       = { 0, 0 };   // เวลาอัปเดตล่าสุด

bool          wsConnected    = false;
unsigned long lastSensorSend = 0;

const int SENSOR_INTERVAL  = 100;   // ส่งทุก 100ms
const int WIFI_TIMEOUT_MS  = 20000;

// ── การเคลื่อนที่เซอร์โว (ลื่นไหล: ease-in/out + micros resolution) ──
// MG92B (TowerPro): 180° เต็มต้องใช้ช่วงพัลส์ ~500–2400µs
// ⚠️ ถ้าได้ยินเสียงคราง/สั่นค้างที่ปลายสุด (0° หรือ 180°) = ชนขอบกลไก
//    ให้หดช่วงเข้า เช่น 550/2350 → 600/2300 จนเสียงหาย
const float SERVO_SPEED_DPS = 90.0;   // ความเร็วเฉลี่ย (องศา/วินาที) — เพิ่ม = เร็วขึ้น
const int   SERVO_UPDATE_MS = 10;     // อัปเดตทุกกี่ ms — ยิ่งน้อยยิ่งลื่น (10ms = 100Hz)
const int   SERVO_MIN_US    = 550;    // ความกว้างพัลส์ที่ 0°   (MG92B เต็มช่วง)
const int   SERVO_MAX_US    = 2350;   // ความกว้างพัลส์ที่ 180° (MG92B เต็มช่วง)

// ── Sensor Read ──────────────────────────────────────────────
float readFSRNewton(int ch)   { return (4095 - analogRead(PIN_FSR[ch]))  * (50.0 / 4095.0); }
int   readFlexDegrees(int ch) { return constrain(map(analogRead(PIN_FLEX[ch]), 1000, 3500, 0, 90), 0, 90); }

// อ่านไมค์ INMP441 ผ่าน I2S → คำนวณ RMS → คืนค่าระดับเสียง 0–100%
// แบบ non-blocking: อ่านเฉพาะข้อมูลที่พร้อมแล้ว ไม่หยุดรอ (กัน loop ค้าง)
int readMicLevel() {
  int avail = I2S.available();
  if (avail <= 0) return micLevel;      // ยังไม่มีข้อมูล → คงค่าเดิม ไม่บล็อก
  size_t want  = min((size_t)avail, sizeof(micBuf));
  size_t bytes = I2S.readBytes((char*)micBuf, want);
  int n = bytes / 2;
  if (n == 0) return micLevel;
  double sum = 0;
  for (int i = 0; i < n; i++) sum += (double)micBuf[i] * micBuf[i];
  double rms = sqrt(sum / n);
  int level = (int)(rms * 100.0 / MIC_REF);
  return constrain(level, 0, 100);
}

// เขียนมุม (float) เป็นความกว้างพัลส์ — ละเอียดกว่า write(int) จึงลื่นกว่า
void writeServoMicros(int ch, float deg) {
  if (!myServo[ch].attached()) return;
  float us = SERVO_MIN_US + (deg / 180.0) * (SERVO_MAX_US - SERVO_MIN_US);
  myServo[ch].writeMicroseconds((int)roundf(us));
}

// ── Servo Control (non-blocking) ต่อช่อง ─────────────────────
// ตั้งเป้าหมาย + คำนวณระยะเวลา — การขยับจริงทำใน stepServos() แบบ ease-in/out
// หมายเหตุ: ไม่ detach อีกต่อไป — จับตำแหน่งค้างตลอด กันกลไกดันกลับ
// แล้วสะบัดตอนสั่งครั้งถัดไป (ไม่มี position feedback)
void moveServo(int ch, int angle, bool lock) {
  if (ch < 0 || ch >= NUM_CH) return;
  int target = constrain(angle, 0, 180);
  servoStart[ch]     = servoPos[ch];
  servoTarget[ch]    = target;
  servoMoveStart[ch] = millis();
  servoMoveDur[ch]   = (fabs(target - servoPos[ch]) / SERVO_SPEED_DPS) * 1000.0;  // ระยะเวลา(ms)
  servoLocked[ch]    = lock;
  if (!myServo[ch].attached()) {
    myServo[ch].attach(PIN_SERVO[ch], SERVO_MIN_US, SERVO_MAX_US);
    writeServoMicros(ch, servoPos[ch]);   // เริ่มพัลส์จากตำแหน่งล่าสุดที่รู้ ไม่กระโดด
  }
  Serial.printf("[SERVO] ch=%d target=%d lock=%d\n", ch, target, lock);
}

// สั่งทุกช่อง (เมื่อ command ไม่ระบุ ch)
void moveAll(int angle, bool lock) {
  for (int ch = 0; ch < NUM_CH; ch++) moveServo(ch, angle, lock);
}

// ── ขยับเซอร์โวเข้าหาเป้าหมายแบบ ease-in/out (นุ่มลื่น) ──────
void stepServos() {
  unsigned long now = millis();
  for (int ch = 0; ch < NUM_CH; ch++) {
    // ยังไม่ถึงเป้าหมาย → interpolate ตำแหน่งด้วย smoothstep
    if (servoPos[ch] != (float)servoTarget[ch] && now - servoLastUpd[ch] >= SERVO_UPDATE_MS) {
      servoLastUpd[ch] = now;
      float t = servoMoveDur[ch] > 0 ? (now - servoMoveStart[ch]) / servoMoveDur[ch] : 1.0;
      if (t >= 1.0) {
        servoPos[ch] = servoTarget[ch];                    // ถึงแล้ว — ค้างตำแหน่งไว้
      } else {
        float eased = t * t * (3.0 - 2.0 * t);              // smoothstep: เริ่มช้า–เร่ง–ชะลอ
        servoPos[ch] = servoStart[ch] + (servoTarget[ch] - servoStart[ch]) * eased;
      }
      writeServoMicros(ch, servoPos[ch]);
    }
  }
}

// ── Parse & Handle Command จาก Server ───────────────────────
void handleCommand(const String& msg) {
  JsonDocument doc;
  if (deserializeJson(doc, msg) != DeserializationError::Ok) return;

  const char* type = doc["type"];
  if (!type) return;   // ไม่มี key "type" → กัน null deref

  // Manual motor command
  if (strcmp(type, "command") == 0) {
    const char* action    = doc["action"]    | "none";
    int         intensity = doc["intensity"] | 50;
    // ถ้าไม่ระบุ ch → -1 = สั่งทุกช่อง
    int         ch        = doc["ch"].is<int>() ? (int)doc["ch"] : -1;

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

  // Voice command — "จับ" = กำนิ้วเข้าจับ (0°), "ปล่อย" = คลายนิ้วกลับ (180°)
  else if (strcmp(type, "voice") == 0) {
    String text = doc["text"] | "";
    Serial.println("[VOICE] " + text);

    if (text.indexOf("ปล่อย") >= 0) moveAll(180, false);   // ปล่อย = คลายนิ้ว (เช็กก่อน)
    else if (text.indexOf("จับ") >= 0) moveAll(0, true);   // จับ = กำนิ้ว ค้างแรงไว้
  }
}

// ── ส่ง Sensor Data ขึ้น Server (รูปแบบ array 2 ช่อง) ────────
void sendSensorData() {
  if (!wsConnected) return;

  JsonDocument doc;
  doc["type"] = "sensor_data";

  JsonArray fsr   = doc["fsr"].to<JsonArray>();
  JsonArray flex  = doc["flex"].to<JsonArray>();
  JsonArray servo = doc["servo"].to<JsonArray>();
  JsonArray lock  = doc["lock"].to<JsonArray>();
  for (int ch = 0; ch < NUM_CH; ch++) {
    fsr.add(readFSRNewton(ch));
    flex.add(readFlexDegrees(ch));
    servo.add((int)roundf(servoPos[ch]));   // รายงานมุมปัจจุบันจริง (ขยับนุ่ม)
    lock.add(servoLocked[ch]);
  }

  doc["mic"]       = micLevel;   // 0–100% (อัปเดตใน loop)
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
        String msg = String((char*)payload, length);
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

  // Servo init (ทุกช่อง) — ค้างตำแหน่ง 0° ไว้ ไม่ detach
  for (int ch = 0; ch < NUM_CH; ch++) {
    myServo[ch].attach(PIN_SERVO[ch], SERVO_MIN_US, SERVO_MAX_US);
    writeServoMicros(ch, 0);
  }
  delay(500);

  // I2S init สำหรับไมค์ INMP441 (RX อย่างเดียว → dout = -1)
  I2S.setPins(MIC_SCK, MIC_WS, -1, MIC_SD);
  if (!I2S.begin(I2S_MODE_STD, MIC_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("[I2S] init ล้มเหลว! ตรวจสายไมค์ INMP441");
  } else {
    Serial.println("[I2S] INMP441 พร้อมใช้งาน");
  }

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

  stepServos();   // ขยับเซอร์โวเข้าหาเป้าหมายแบบนุ่มลื่น

  // อ่านระดับเสียงต่อเนื่องทุก loop — readBytes จะ pace loop ที่ ~60Hz
  // (เพียงพอต่อ WebSocket) และคอย drain DMA ให้ค่า mic สดเสมอ
  micLevel = readMicLevel();

  // ส่ง sensor ทุก 100ms
  if (millis() - lastSensorSend >= SENSOR_INTERVAL) {
    sendSensorData();
    lastSensorSend = millis();
  }
}
