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
const char* WIFI_SSID     = "OPPO A5 2020 (MIK)";   // ← แก้
const char* WIFI_PASSWORD = "yourfriends";   // ← แก้

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
float         servoPos[NUM_CH]           = { 0, 0 };   // มุมปัจจุบัน (ค่อยๆ ขยับ)
bool          servoLocked[NUM_CH]        = { false, false };
unsigned long servoLastStep[NUM_CH]      = { 0, 0 };   // เวลา step ล่าสุด
unsigned long servoReachedAt[NUM_CH]     = { 0, 0 };   // เวลาที่ถึงเป้าหมาย
bool          servoDetachPending[NUM_CH] = { false, false };

bool          wsConnected    = false;
unsigned long lastSensorSend = 0;

const int SENSOR_INTERVAL  = 100;   // ส่งทุก 100ms
const int SERVO_DETACH_MS  = 600;   // พักหลังถึงเป้าหมายก่อน detach (เฉพาะไม่ล็อก)
const int WIFI_TIMEOUT_MS  = 20000;

// ── ความเร็วเซอร์โว (software stepping) ───────────────────────
// ความเร็ว ≈ SERVO_STEP_DEG ÷ SERVO_STEP_MS  (เช่น 1°/15ms ≈ 66°/วินาที)
const float SERVO_STEP_DEG = 1.0;   // องศาต่อ step — เพิ่ม = เร็วขึ้น (กระตุกขึ้น)
const int   SERVO_STEP_MS  = 15;    // เวลาต่อ step (ms) — เพิ่ม = ช้าลง (นุ่มขึ้น)

// ── Sensor Read ──────────────────────────────────────────────
float readFSRNewton(int ch)   { return (4095 - analogRead(PIN_FSR[ch]))  * (50.0 / 4095.0); }
int   readFlexDegrees(int ch) { return constrain(map(analogRead(PIN_FLEX[ch]), 1000, 3500, 0, 90), 0, 90); }

// อ่านไมค์ INMP441 ผ่าน I2S → คำนวณ RMS → คืนค่าระดับเสียง 0–100%
int readMicLevel() {
  size_t bytes = I2S.readBytes((char*)micBuf, sizeof(micBuf));
  int n = bytes / 2;
  if (n == 0) return micLevel;          // ไม่มีข้อมูล → คงค่าเดิม
  double sum = 0;
  for (int i = 0; i < n; i++) sum += (double)micBuf[i] * micBuf[i];
  double rms = sqrt(sum / n);
  int level = (int)(rms * 100.0 / MIC_REF);
  return constrain(level, 0, 100);
}

// ── Servo Control (non-blocking) ต่อช่อง ─────────────────────
// ตั้งเป้าหมายอย่างเดียว — การขยับจริงทำใน stepServos() เพื่อคุมความเร็ว
void moveServo(int ch, int angle, bool lock) {
  if (ch < 0 || ch >= NUM_CH) return;
  servoTarget[ch] = constrain(angle, 0, 180);
  servoLocked[ch] = lock;
  if (!myServo[ch].attached()) myServo[ch].attach(PIN_SERVO[ch]);
  servoDetachPending[ch] = false;   // จะตั้งใหม่เมื่อขยับถึงเป้าหมาย (ถ้าไม่ล็อก)

  // อยู่ที่เป้าหมายอยู่แล้ว + ไม่ล็อก → เริ่มจับเวลา detach ทันที
  if (!lock && (int)roundf(servoPos[ch]) == servoTarget[ch]) {
    servoDetachPending[ch] = true;
    servoReachedAt[ch]     = millis();
  }
  Serial.printf("[SERVO] ch=%d target=%d lock=%d\n", ch, servoTarget[ch], lock);
}

// สั่งทุกช่อง (เมื่อ command ไม่ระบุ ch)
void moveAll(int angle, bool lock) {
  for (int ch = 0; ch < NUM_CH; ch++) moveServo(ch, angle, lock);
}

// ── ขยับเซอร์โวเข้าหาเป้าหมายทีละ step (คุมความเร็ว) ─────────
void stepServos() {
  unsigned long now = millis();
  for (int ch = 0; ch < NUM_CH; ch++) {
    // ยังไม่ถึงเป้าหมาย → ขยับทีละ step ตามจังหวะเวลา
    if ((int)roundf(servoPos[ch]) != servoTarget[ch] && now - servoLastStep[ch] >= SERVO_STEP_MS) {
      servoLastStep[ch] = now;
      float diff = servoTarget[ch] - servoPos[ch];
      servoPos[ch] += constrain(diff, -SERVO_STEP_DEG, SERVO_STEP_DEG);
      if (myServo[ch].attached()) myServo[ch].write((int)roundf(servoPos[ch]));

      // เพิ่งถึงเป้าหมาย + ไม่ล็อก → เริ่มจับเวลา detach
      if ((int)roundf(servoPos[ch]) == servoTarget[ch] && !servoLocked[ch]) {
        servoDetachPending[ch] = true;
        servoReachedAt[ch]     = now;
      }
    }

    // detach หลังถึงเป้าหมายและพัก SERVO_DETACH_MS (เฉพาะไม่ล็อก)
    if (servoDetachPending[ch] && now - servoReachedAt[ch] >= SERVO_DETACH_MS) {
      myServo[ch].detach();
      servoDetachPending[ch] = false;
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

  // Voice command (สั่งทุกช่องพร้อมกัน)
  else if (strcmp(type, "voice") == 0) {
    String text = doc["text"] | "";
    Serial.println("[VOICE] " + text);

    if (text.indexOf("เปิด")  >= 0) moveAll(0,   false);
    if (text.indexOf("ปิด")   >= 0 || text.indexOf("กำ") >= 0) moveAll(180, true);
    if (text.indexOf("กลาง")  >= 0) moveAll(90,  true);
    if (text.indexOf("ล็อก")  >= 0) for (int ch = 0; ch < NUM_CH; ch++) moveServo(ch, (int)roundf(servoPos[ch]), true);
    if (text.indexOf("ปล่อย") >= 0) for (int ch = 0; ch < NUM_CH; ch++) moveServo(ch, servoTarget[ch], false);
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

  // Servo init (ทุกช่อง)
  for (int ch = 0; ch < NUM_CH; ch++) {
    myServo[ch].attach(PIN_SERVO[ch]);
    myServo[ch].write(0);
  }
  delay(500);
  for (int ch = 0; ch < NUM_CH; ch++) myServo[ch].detach();

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

  stepServos();   // ขยับเซอร์โวตามความเร็วที่ตั้ง + จัดการ detach

  // อ่านระดับเสียงต่อเนื่องทุก loop — readBytes จะ pace loop ที่ ~60Hz
  // (เพียงพอต่อ WebSocket) และคอย drain DMA ให้ค่า mic สดเสมอ
  micLevel = readMicLevel();

  // ส่ง sensor ทุก 100ms
  if (millis() - lastSensorSend >= SENSOR_INTERVAL) {
    sendSensorData();
    lastSensorSend = millis();
  }
}
