/*
 * ============================================================
 *  Exoskeleton WebSocket Server — server.js
 *  Node.js + ws library
 *  Deploy: Render (free) → wss://exoskeleton-server.onrender.com
 *
 *  ทำหน้าที่ส่งต่อข้อความอย่างเดียว:
 *    ESP32 (/esp32) ──sensor_data──→ Dashboard ทุกตัว (/dashboard)
 *    Dashboard ──command / voice──→ ESP32
 * ============================================================
 */

const WebSocket = require("ws");
const express   = require("express");
const cors      = require("cors");

const app  = express();
const PORT = process.env.PORT || 3001;

app.use(cors());

// ── Health check + หน้าวินิจฉัย (Render เรียกเช็คว่า server ยังรันอยู่) ──
app.get("/", (req, res) => {
  res.json({
    status : "ok",
    clients: {
      esp32    : esp32IsLive()   ? "connected" : "disconnected",
      dashboard: dashboardClients.size + " connected",
    },
    esp32LastSeen: esp32LastSeen
      ? Math.round((Date.now() - esp32LastSeen) / 1000) + "s ago"
      : "never",
    esp32SocketOpen: !!esp32Client,
    uptime: Math.floor(process.uptime()) + "s",
  });
});

// ── HTTP Server + WebSocket Server ───────────────────────────
const httpServer = app.listen(PORT, () => {
  console.log(`[Server] HTTP running on port ${PORT}`);
});

const wss = new WebSocket.Server({ server: httpServer });

// ── Client Registry ──────────────────────────────────────────
let esp32Client      = null;          // มีได้แค่ 1 ตัว
let esp32LastSeen    = 0;             // เวลาที่ได้รับข้อมูลจากบอร์ดครั้งล่าสุด (0 = ยังไม่เคย)
let esp32ConnectedAt = 0;             // เวลาที่ socket ต่อเข้ามา (ใช้เป็นช่วงผ่อนผัน)
let dashboardClients = new Set();     // มีได้หลายตัว

// บอร์ดส่ง sensor_data ทุก 100 ms — ถ้าเงียบเกินเท่านี้ถือว่าไม่มีบอร์ดจริง
const ESP32_SILENCE_MS = 10000;

// ── Helper: ส่ง JSON ──────────────────────────────────────────
function sendJSON(client, data) {
  if (client && client.readyState === WebSocket.OPEN) {
    client.send(JSON.stringify(data));
  }
}

// ── Helper: บอร์ด "ออนไลน์จริง" ไหม ──────────────────────────
// ตัดสินจากข้อมูลที่ส่งเข้ามาจริง ไม่ใช่จากสถานะ socket
// เพราะ proxy ของ Render ตอบ pong แทนบอร์ดที่ตายไปแล้วได้
// ทำให้ ping/pong ระดับ WebSocket เชื่อถือไม่ได้ (เกิด "ทะเบียนผี")
function esp32IsLive() {
  if (!esp32Client || !esp32LastSeen) return false;   // ยังไม่เคยส่งข้อมูล = ยังไม่นับ
  return (Date.now() - esp32LastSeen) < ESP32_SILENCE_MS;
}

// ── Helper: broadcast ไปทุก Dashboard ────────────────────────
function broadcastToDashboards(data) {
  const msg = JSON.stringify(data);
  dashboardClients.forEach((client) => {
    if (client.readyState === WebSocket.OPEN) client.send(msg);
  });
}

// ── WebSocket Connection Handler ──────────────────────────────
wss.on("connection", (ws, req) => {
  const ip   = req.socket.remoteAddress;
  // ตัด query string ออกก่อนเทียบ และรับเฉพาะ 2 path ที่รู้จัก
  // เดิมใช้ "ไม่ใช่ /esp32 = dashboard" ทำให้บอตที่มาสแกนถูกนับเป็น dashboard
  // และถ้าบอร์ดต่อมาพร้อม query string จะถูกมองเป็น dashboard แล้วข้อมูลหายเงียบ ๆ
  const path = (req.url || "").split("?")[0];
  const type = path === "/esp32" ? "esp32" : path === "/dashboard" ? "dashboard" : null;
  if (!type) {
    console.log(`[WS] ปฏิเสธการเชื่อมต่อ path "${path}" | IP: ${ip}`);
    ws.close(1008, "unknown path");
    // proxy ของ Render ไม่ส่ง close frame ต่อให้ทันที ทำให้ socket ค้างเปิด
    // กินสล็อตทิ้งไว้ — บังคับตัดซ้ำถ้ายังไม่ปิดเองใน 1 วินาที
    setTimeout(() => { if (ws.readyState !== WebSocket.CLOSED) ws.terminate(); }, 1000);
    return;
  }

  console.log(`[WS] New connection — type: ${type} | IP: ${ip}`);

  // ── ลงทะเบียน client ──
  if (type === "esp32") {
    // ลงทะเบียนตัวใหม่ "ก่อน" ตัดตัวเก่าเสมอ — event close ของตัวเก่าจะได้
    // เห็นว่าตัวเองไม่ใช่ตัวที่ลงทะเบียนอยู่ แล้วข้ามการล้างทะเบียนไป
    // ใช้ terminate() ไม่ใช่ close() เพราะ socket เก่ามักเป็น TCP ที่ตายแล้ว
    // (บอร์ดหลุด WiFi) จะไม่ตอบ close handshake ทำให้ค้างรอจนถึง keepalive
    const prevEsp32  = esp32Client;
    esp32Client      = ws;
    esp32LastSeen    = 0;            // ตัวใหม่ต้องพิสูจน์ตัวเองด้วยข้อมูลจริง
    esp32ConnectedAt = Date.now();
    if (prevEsp32) {
      prevEsp32.terminate();
      console.log("[WS] Previous ESP32 replaced");
    }

    // ยังไม่แจ้งว่า online ตรงนี้ — รอให้บอร์ดส่งข้อมูลจริงมาก่อน
    // (socket ที่เปิดค้างโดยไม่มีบอร์ดจริงจะไม่ทำให้ dashboard ขึ้น Online)
    console.log("[WS] ESP32 socket registered — รอข้อมูลยืนยัน");
  } else {
    dashboardClients.add(ws);
    console.log(`[WS] Dashboard added (total: ${dashboardClients.size})`);

    // ส่งสถานะ ESP32 ปัจจุบันให้ Dashboard ใหม่
    sendJSON(ws, {
      type  : "esp32_status",
      status: esp32IsLive() ? "connected" : "disconnected",
    });
  }

  // ── รับ Message ──────────────────────────────────────────────
  ws.on("message", (raw) => {
    let data;
    try {
      data = JSON.parse(raw);
    } catch {
      console.warn("[WS] Invalid JSON:", raw.toString().slice(0, 100));
      return;
    }

    // ไม่ log sensor_data — บอร์ดส่ง 10 ครั้ง/วินาที จะท่วม log ของ Render
    // (~860,000 บรรทัด/วัน) และเปลืองแรงเครื่องบน free tier
    if (data.type !== "sensor_data") console.log(`[WS] ${type} → ${data.type}`);

    if (type === "esp32") {
      if (ws !== esp32Client) return;   // socket เก่าที่ถูกแทนที่ไปแล้ว

      // ข้อมูลจริงจากบอร์ด = หลักฐานว่ายังมีชีวิต
      const wasLive = esp32IsLive();
      esp32LastSeen = Date.now();
      if (!wasLive) {
        broadcastToDashboards({ type: "esp32_status", status: "connected" });
        console.log("[WS] ESP32 online — ยืนยันจากข้อมูลจริง");
      }

      // ESP32 ส่ง sensor_data → relay ไปทุก Dashboard
      broadcastToDashboards(data);

    } else {
      // Dashboard ส่ง command / voice → relay ไป ESP32
      if (data.type === "command" || data.type === "voice") {
        if (esp32IsLive()) {
          sendJSON(esp32Client, data);
        } else {
          // ESP32 ไม่ได้ต่ออยู่ → แจ้ง Dashboard กลับ
          sendJSON(ws, {
            type   : "error",
            message: "ESP32 not connected",
          });
        }
      }
    }
  });

  // ── Disconnect ────────────────────────────────────────────────
  ws.on("close", (code, reason) => {
    if (type === "esp32") {
      // socket เก่าที่ถูกตัวใหม่แทนที่ไปแล้ว: event close มาถึงทีหลัง
      // ถ้าปล่อยให้ล้าง esp32Client จะเป็นการลบทะเบียนของบอร์ดตัวใหม่ทิ้ง
      // → dashboard ขึ้น Offline และสั่งงานไม่ได้ ทั้งที่ข้อมูลยังไหลอยู่
      if (esp32Client !== ws) {
        console.log("[WS] Stale ESP32 socket closed — ทะเบียนตัวใหม่ยังอยู่");
        return;
      }
      const wasLive    = esp32LastSeen > 0;   // เคยแจ้ง Online ไปแล้วหรือยัง
      esp32Client      = null;
      esp32LastSeen    = 0;
      esp32ConnectedAt = 0;
      // แจ้ง Offline เฉพาะตอนที่เคยขึ้น Online จริง ๆ จะได้ไม่รบกวน dashboard
      // ด้วยข้อความจาก socket ผีที่ไม่เคยส่งข้อมูลอะไรมาเลย
      if (wasLive) {
        broadcastToDashboards({ type: "esp32_status", status: "disconnected" });
        console.log("[WS] ESP32 disconnected");
      } else {
        console.log("[WS] ESP32 socket ปิด (ไม่เคยส่งข้อมูล — ไม่ใช่บอร์ดจริง)");
      }
    } else {
      dashboardClients.delete(ws);
      console.log(`[WS] Dashboard disconnected (remaining: ${dashboardClients.size})`);
    }
  });

  ws.on("error", (err) => {
    console.error(`[WS] Error (${type}):`, err.message);
  });

  // ── Ping-Pong keepalive (เก็บกวาด dashboard ที่ปิดไปแบบไม่บอกลา) ─
  // หมายเหตุ: ใช้ตรวจบอร์ดไม่ได้ เพราะ proxy ของ Render ตอบ pong แทน
  // การตรวจบอร์ดอยู่ที่ livenessCheck ด้านล่างซึ่งวัดจากข้อมูลจริง
  ws.isAlive = true;
  ws.on("pong", () => { ws.isAlive = true; });
});

// ── Keepalive interval ────────────────────────────────────────
const keepalive = setInterval(() => {
  wss.clients.forEach((ws) => {
    if (!ws.isAlive) { ws.terminate(); return; }
    ws.isAlive = false;
    ws.ping();
  });
}, 30000); // ทุก 30 วิ

// ── ตรวจว่าบอร์ดเงียบหายไปไหม ─────────────────────────────────
// ping/pong พึ่งไม่ได้เพราะ proxy ตอบแทนได้ จึงวัดจากข้อมูลจริงแทน
// บอร์ดที่ทำงานปกติส่งทุก 100 ms — เงียบเกิน 10 วิ = ตัดทิ้งเลย
const livenessCheck = setInterval(() => {
  if (!esp32Client) return;
  // ยังไม่เคยส่งข้อมูล → นับจากตอนต่อเข้ามา (ให้เวลาพิสูจน์ตัวเอง)
  const silent = Date.now() - (esp32LastSeen || esp32ConnectedAt);
  if (silent < ESP32_SILENCE_MS) return;

  console.log(`[WS] ESP32 เงียบมา ${Math.round(silent / 1000)}s — ตัดการเชื่อมต่อ`);
  const ghost = esp32Client;
  // เคยส่งข้อมูลมาก่อน = เคยแจ้ง dashboard ว่า Online ไปแล้ว ต้องแจ้ง Offline กลับ
  // (ใช้ esp32IsLive() ตรงนี้ไม่ได้ เพราะ ณ จุดนี้มันเป็น false เสมอโดยนิยาม)
  const wasLive = esp32LastSeen > 0;
  esp32Client      = null;
  esp32LastSeen    = 0;
  esp32ConnectedAt = 0;
  ghost.terminate();
  if (wasLive) {
    broadcastToDashboards({ type: "esp32_status", status: "disconnected" });
  }
}, 5000);

wss.on("close", () => { clearInterval(keepalive); clearInterval(livenessCheck); });

console.log("[Server] WebSocket ready");
console.log("[Server] ESP32  → connect to /esp32");
console.log("[Server] Dashboard → connect to /dashboard");
