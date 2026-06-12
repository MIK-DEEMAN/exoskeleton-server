/*
 * ============================================================
 *  EMF Voltage Monitor — WebSocket Relay Server
 *  Node.js + ws + Express
 *  Deploy: Railway → wss://YOUR_APP.railway.app
 *
 *  Endpoints:
 *    /esp32     ← ESP32 connects here (1 client only)
 *    /dashboard ← Browser clients connect here (N clients)
 * ============================================================
 */

const WebSocket = require("ws");
const express   = require("express");
const cors      = require("cors");

const app  = express();
const PORT = process.env.PORT || 3001;

app.use(cors());
app.use(express.json());

// ── Health check ──────────────────────────────────────────────
app.get("/", (req, res) => {
  res.json({
    service : "EMF Voltage Monitor Server",
    status  : "ok",
    clients : {
      esp32    : esp32Client      ? "connected" : "disconnected",
      dashboard: dashboardClients.size + " connected",
    },
    uptime  : Math.floor(process.uptime()) + "s",
    lastData: lastData ? "available" : "none",
  });
});

// ── HTTP + WebSocket Server ───────────────────────────────────
const httpServer = app.listen(PORT, () =>
  console.log(`[Server] HTTP on port ${PORT}`)
);

const wss = new WebSocket.Server({ server: httpServer });

// ── Client Registry ───────────────────────────────────────────
let esp32Client      = null;
let dashboardClients = new Set();
let lastData         = null;   // cache last sensor packet for new dashboards

// ── Helpers ───────────────────────────────────────────────────
function sendJSON(client, data) {
  if (client && client.readyState === WebSocket.OPEN)
    client.send(JSON.stringify(data));
}

function broadcastToDashboards(data) {
  const msg = JSON.stringify(data);
  dashboardClients.forEach(c => {
    if (c.readyState === WebSocket.OPEN) c.send(msg);
  });
}

// ── Connection Handler ─────────────────────────────────────────
wss.on("connection", (ws, req) => {
  const type = req.url === "/esp32" ? "esp32" : "dashboard";
  const ip   = req.socket.remoteAddress;
  console.log(`[WS] New ${type} | IP: ${ip}`);

  if (type === "esp32") {
    if (esp32Client) { esp32Client.close(); }
    esp32Client = ws;
    broadcastToDashboards({ type: "esp32_status", status: "connected" });
    console.log("[WS] ESP32 registered");
  } else {
    dashboardClients.add(ws);
    console.log(`[WS] Dashboard added (total: ${dashboardClients.size})`);

    sendJSON(ws, {
      type  : "esp32_status",
      status: esp32Client ? "connected" : "disconnected",
    });
    if (lastData) sendJSON(ws, lastData);
  }

  // ── Messages ─────────────────────────────────────────────────
  ws.on("message", raw => {
    let data;
    try { data = JSON.parse(raw); }
    catch { console.warn("[WS] Bad JSON:", raw.toString().slice(0, 80)); return; }

    if (type === "esp32" && data.type === "sensor_data") {
      lastData = data;
      broadcastToDashboards(data);

    } else if (type === "dashboard") {
      // Dashboard → ESP32: reset command
      if (data.type === "reset") {
        lastData = null;
        if (esp32Client) sendJSON(esp32Client, data);
        else sendJSON(ws, { type: "error", message: "ESP32 not connected" });
      }
    }
  });

  // ── Disconnect ────────────────────────────────────────────────
  ws.on("close", () => {
    if (type === "esp32") {
      esp32Client = null;
      broadcastToDashboards({ type: "esp32_status", status: "disconnected" });
      console.log("[WS] ESP32 disconnected");
    } else {
      dashboardClients.delete(ws);
      console.log(`[WS] Dashboard removed (remaining: ${dashboardClients.size})`);
    }
  });

  ws.on("error", err => console.error(`[WS] Error (${type}):`, err.message));

  // ── Keepalive ─────────────────────────────────────────────────
  ws.isAlive = true;
  ws.on("pong", () => { ws.isAlive = true; });
});

const keepalive = setInterval(() => {
  wss.clients.forEach(ws => {
    if (!ws.isAlive) { ws.terminate(); return; }
    ws.isAlive = false;
    ws.ping();
  });
}, 30_000);

wss.on("close", () => clearInterval(keepalive));

console.log("[Server] WebSocket ready");
console.log("[Server] ESP32     → connect to /esp32");
console.log("[Server] Dashboard → connect to /dashboard");
