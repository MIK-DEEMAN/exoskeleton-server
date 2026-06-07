/*
 * ============================================================
 *  Exoskeleton WebSocket Server — server.js
 *  Node.js + ws library
 *  Deploy: Railway (ฟรี) → ได้ wss://xxx.railway.app
 * ============================================================
 */

const WebSocket = require("ws");
const express   = require("express");
const cors      = require("cors");

const app  = express();
const PORT = process.env.PORT || 3001;

app.use(cors());
app.use(express.json());

// ── Health check (Railway ใช้ตรวจสอบว่า server ยังรันอยู่) ──
app.get("/", (req, res) => {
  res.json({
    status : "ok",
    clients: {
      esp32    : esp32Client     ? "connected" : "disconnected",
      dashboard: dashboardClients.size + " connected",
    },
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
let dashboardClients = new Set();     // มีได้หลายตัว

// ── Helper: ส่ง JSON ──────────────────────────────────────────
function sendJSON(client, data) {
  if (client && client.readyState === WebSocket.OPEN) {
    client.send(JSON.stringify(data));
  }
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
  const type = req.url === "/esp32" ? "esp32" : "dashboard";

  console.log(`[WS] New connection — type: ${type} | IP: ${ip}`);

  // ── ลงทะเบียน client ──
  if (type === "esp32") {
    // ถ้ามี ESP32 เก่าอยู่แล้ว → ปิดก่อน
    if (esp32Client) {
      esp32Client.close();
      console.log("[WS] Previous ESP32 disconnected");
    }
    esp32Client = ws;

    // แจ้ง Dashboard ว่า ESP32 online
    broadcastToDashboards({ type: "esp32_status", status: "connected" });
    console.log("[WS] ESP32 registered");
  } else {
    dashboardClients.add(ws);
    console.log(`[WS] Dashboard added (total: ${dashboardClients.size})`);

    // ส่งสถานะ ESP32 ปัจจุบันให้ Dashboard ใหม่
    sendJSON(ws, {
      type  : "esp32_status",
      status: esp32Client ? "connected" : "disconnected",
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

    console.log(`[WS] ${type} → ${data.type}`);

    if (type === "esp32") {
      // ESP32 ส่ง sensor_data → relay ไปทุก Dashboard
      broadcastToDashboards(data);

    } else {
      // Dashboard ส่ง command / voice → relay ไป ESP32
      if (data.type === "command" || data.type === "voice") {
        if (esp32Client) {
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
      esp32Client = null;
      broadcastToDashboards({ type: "esp32_status", status: "disconnected" });
      console.log("[WS] ESP32 disconnected");
    } else {
      dashboardClients.delete(ws);
      console.log(`[WS] Dashboard disconnected (remaining: ${dashboardClients.size})`);
    }
  });

  ws.on("error", (err) => {
    console.error(`[WS] Error (${type}):`, err.message);
  });

  // ── Ping-Pong keepalive (ป้องกัน Railway ตัด idle connection) ─
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

wss.on("close", () => clearInterval(keepalive));

console.log("[Server] WebSocket ready");
console.log("[Server] ESP32  → connect to /esp32");
console.log("[Server] Dashboard → connect to /dashboard");
