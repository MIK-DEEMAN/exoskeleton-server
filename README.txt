╔═══════════════════════════════════════════════════════════════════╗
║                                                                   ║
║   🖐  EXOSKELETON CLOUD SYSTEM — All-in-One Bundle                ║
║                                                                   ║
║   ระบบ Finger Exoskeleton ควบคุมผ่าน Cloud จากทุกที่              ║
║                                                                   ║
╚═══════════════════════════════════════════════════════════════════╝


📦 สิ่งที่อยู่ในชุดนี้ (3 โฟลเดอร์)

   1-server/           → โค้ด Server (Deploy ขึ้น Railway)
      ├── server.js
      ├── package.json
      └── .gitignore

   2-esp32/            → โค้ด ESP32 (Upload เข้าบอร์ด)
      └── main_cloud.ino

   3-dashboard/        → หน้าเว็บ Dashboard (Deploy ขึ้น Vercel)
      └── index.html


🎯 จะได้อะไรเมื่อทำเสร็จ?

   ESP32 (Hotspot) ──→ Server (Railway) ──→ Dashboard (Vercel)
   
   เปิด Dashboard URL บนมือถือไหนก็ได้ในโลก เห็นค่า sensor real-time
   และควบคุม Servo จากที่ไหนก็ได้


⏰ เวลารวม: 3-5 ชั่วโมง (สำหรับมือใหม่)


═══════════════════════════════════════════════════════════════════
  ⚡ QUICK START — ทำตามนี้ทีละข้อ
═══════════════════════════════════════════════════════════════════


┌──────────────────────────────────────────────────────────────┐
│  เตรียมก่อนเริ่ม (ทำครั้งเดียว)                              │
└──────────────────────────────────────────────────────────────┘

  □ ติดตั้ง Node.js LTS  → nodejs.org
  □ ติดตั้ง Git          → git-scm.com
  □ ติดตั้ง Arduino IDE  → arduino.cc
  □ สมัคร GitHub         → github.com/signup
  □ ตั้งค่า Git:
       git config --global user.name "ชื่อคุณ"
       git config --global user.email "you@gmail.com"
  □ สร้าง GitHub Token (เก็บไว้!):
       GitHub → Settings → Developer settings 
       → Personal access tokens → Tokens (classic)
       → Generate new token (classic)
       → ติ๊ก "repo" → Generate
       → คัดลอกเก็บไว้ทันที (ออกจากหน้านี้แล้วหายไป!)


═══════════════════════════════════════════════════════════════════
  PART 1 — DEPLOY SERVER บน RAILWAY  (45 นาที)
═══════════════════════════════════════════════════════════════════

🎯 จุดมุ่งหมาย: ได้ URL Server ที่เปิดได้ตลอด 24 ชม.


▸ Step 1.1 — ทดสอบ Server ในเครื่องก่อน

   เปิด Command Prompt (Windows) หรือ Terminal (Mac)

   $ cd Desktop                     ← Mac ใช้: cd ~/Desktop
   $ mkdir exoskeleton-server
   $ cd exoskeleton-server

   เอาไฟล์ 3 ตัวจากโฟลเดอร์ 1-server/ มาวางในนี้:
     server.js, package.json, .gitignore

   $ npm install                    ← รอ 1-2 นาที
   $ node server.js

   ✅ ต้องเห็น: [Server] HTTP running on port 3001

   เปิด browser → http://localhost:3001
   ✅ ต้องเห็น: {"status":"ok",...}

   กด Ctrl+C เพื่อหยุด Server


▸ Step 1.2 — Push ขึ้น GitHub

   ไปที่ github.com → "+" → New repository
   - ชื่อ: exoskeleton-server
   - Public  
   - ⚠️ อย่าติ๊ก "Add README"
   - กด Create repository

   กลับมา Terminal:

   $ git init
   $ git add .
   $ git commit -m "initial server"
   $ git branch -M main
   $ git remote add origin https://github.com/USERNAME/exoskeleton-server.git
        (เปลี่ยน USERNAME เป็นของจริง)
   $ git push -u origin main

   ถาม Username/Password:
   - Username: GitHub username
   - Password: ใส่ Token (ghp_xxx...)


▸ Step 1.3 — Deploy บน Railway

   1. ไปที่ railway.app → Login with GitHub
   2. "+ New Project" → "Deploy from GitHub repo"
   3. เลือก exoskeleton-server → Deploy
   4. รอ 2-3 นาที (ดูที่ tab Deployments)
   5. กดเข้าโปรเจ็ค → Settings → Networking
   6. กด "Generate Domain"
   7. ได้ URL เช่น:
      exoskeleton-server-production-xxxx.up.railway.app


▸ Step 1.4 — ทดสอบ

   เปิด browser → URL Railway (ใส่ https:// ข้างหน้า)
   ✅ ต้องเห็น: {"status":"ok","clients":{"esp32":"disconnected",...}}

📝 จด URL นี้ไว้! → __________________________________________
                    (ตัวอย่าง: exoskeleton-server-xxx.up.railway.app)


═══════════════════════════════════════════════════════════════════
  PART 2 — UPLOAD ESP32 FIRMWARE  (45 นาที)
═══════════════════════════════════════════════════════════════════

🎯 จุดมุ่งหมาย: ESP32 ส่งข้อมูลขึ้น Server ได้ผ่าน Hotspot


▸ Step 2.1 — ติดตั้ง Library

   เปิด Arduino IDE
   → Sketch → Include Library → Manage Libraries
   ติดตั้งทีละตัว (เช็คชื่อผู้พัฒนาให้ตรง):

   ✓ WebSockets   by Markus Sattler        ← ไม่ใช่ตัวอื่น!
   ✓ ArduinoJson  by Benoit Blanchon
   ✓ ESP32Servo   by Kevin Harrington


▸ Step 2.2 — แก้โค้ด 3 จุด

   เปิดไฟล์ main_cloud.ino ใน Arduino IDE
   
   หาบรรทัดเหล่านี้แล้วแก้:

   const char* WIFI_SSID     = "ชื่อ Hotspot มือถือ";
   const char* WIFI_PASSWORD = "รหัสผ่าน Hotspot";
   const char* WS_HOST       = "exoskeleton-server-xxx.up.railway.app";
                                ↑ ไม่ใส่ https:// ไม่ใส่ / ปิดท้าย


▸ Step 2.3 — เปิด Hotspot มือถือ + Upload

   1. เปิด Hotspot มือถือทิ้งไว้
   2. เสียบ ESP32 เข้าคอม
   3. Tools → Board → ESP32 Dev Module
   4. Tools → Port → เลือก COM (หรือ /dev/cu.xxx)
   5. Tools → Partition Scheme → Huge APP (3MB No OTA)
   6. กด Upload (→)
   7. ถ้าค้าง "Connecting..." → กดปุ่ม BOOT บน ESP32 ค้างไว้


▸ Step 2.4 — เช็คการเชื่อมต่อ

   เปิด Serial Monitor (Ctrl+Shift+M) → baud 115200
   กด Reset บน ESP32

   ✅ ต้องเห็น:
   [WiFi] Connected: 192.168.x.x
   [WS] Connected to exoskeleton-server-xxx.up.railway.app/esp32

   เปิด URL Railway → Refresh
   ✅ ต้องเห็น: "esp32":"connected"


═══════════════════════════════════════════════════════════════════
  PART 3 — DEPLOY DASHBOARD บน VERCEL  (30 นาที)
═══════════════════════════════════════════════════════════════════

🎯 จุดมุ่งหมาย: ได้ URL Dashboard เปิดจากทุกที่ในโลก


▸ Step 3.1 — แก้ Dashboard URL

   เปิดไฟล์ index.html ในโฟลเดอร์ 3-dashboard/
   ใช้ Notepad หรือ VS Code (ไม่ใช่ Word!)

   ค้นหา (Ctrl+F): SERVER_URL
   แก้บรรทัดนี้:

   const SERVER_URL = "wss://exoskeleton-server-xxx.up.railway.app/dashboard";
                       ↑ wss:// มี s ห้ามลืม!  
                                                              ↑ ต้องมี /dashboard


▸ Step 3.2 — ทดสอบในเครื่องก่อน (สำคัญ!)

   ดับเบิ้ลคลิก index.html → เปิดด้วย Chrome
   
   ✅ ต้องเห็น:
   - มุมขวาบน "Server Connected" (เขียว)
   - ESP32 "Online" (เขียว)
   - ค่า FSR/Flex อัปเดต real-time
   - กดปุ่ม "กำมือ" → Servo หมุน


▸ Step 3.3 — Push ขึ้น GitHub

   ไปที่ github.com → New repository
   - ชื่อ: exoskeleton-dashboard
   - Public, อย่าติ๊ก Add README

   เปิด Terminal:

   $ cd Desktop                     ← Mac: cd ~/Desktop
   $ mkdir exoskeleton-dashboard
   $ cd exoskeleton-dashboard
   
   เอาไฟล์ index.html (ที่แก้แล้ว) วางในโฟลเดอร์นี้

   $ git init
   $ git add .
   $ git commit -m "initial dashboard"
   $ git branch -M main
   $ git remote add origin https://github.com/USERNAME/exoskeleton-dashboard.git
   $ git push -u origin main
        (ใส่ Token เป็น password เหมือนเดิม)


▸ Step 3.4 — Deploy บน Vercel

   1. ไปที่ vercel.com → Login with GitHub
   2. Add New → Project
   3. เลือก exoskeleton-dashboard → Import
   4. Framework: Other (ไม่ต้องเปลี่ยน)
   5. กด Deploy → รอ 30-60 วิ
   6. ได้ URL เช่น: exoskeleton-dashboard.vercel.app


▸ Step 3.5 — ทดสอบครบวงจร

   เปิด URL Vercel ใน Chrome (มือถือก็ได้)
   ✅ Server Connected (เขียว)
   ✅ ESP32 Online (เขียว)  
   ✅ ค่า sensor อัปเดต
   ✅ กดปุ่ม → Servo หมุน
   ✅ Voice Command ทำงาน (Chrome)

📝 จด URL Dashboard → __________________________________________


═══════════════════════════════════════════════════════════════════
  💡 ทุกครั้งที่ใช้งานจริง (หลัง deploy แล้ว)
═══════════════════════════════════════════════════════════════════

   1. เปิด Hotspot มือถือ
   2. เสียบไฟ ESP32 → รอ 10 วิ ให้ connect
   3. เปิด URL Dashboard บนมือถือ/laptop
   4. พร้อมใช้งาน!

   ไม่ต้องทำอะไรบนคอมพิวเตอร์อีกเลย


═══════════════════════════════════════════════════════════════════
  🔄 ถ้าต้องแก้โค้ด deploy ใหม่
═══════════════════════════════════════════════════════════════════

   Server / Dashboard (Railway / Vercel auto-deploy ให้):
      $ git add .
      $ git commit -m "อธิบายการแก้"
      $ git push
      
      Railway/Vercel จะ rebuild เองใน 1-3 นาที

   ESP32:
      แก้ในไฟล์ .ino → กด Upload (→)


═══════════════════════════════════════════════════════════════════
  🚨 ปัญหาที่พบบ่อย — แก้ตรงนี้ทันที
═══════════════════════════════════════════════════════════════════

  ▸ git push ขึ้น "Authentication failed"
    → ใส่ Token ผิด (ต้อง ghp_xxx ไม่ใช่ password GitHub)

  ▸ ESP32 ค้าง "Connecting..."
    → กดปุ่ม BOOT บน ESP32 ค้างไว้ตอน upload

  ▸ ESP32 Serial Monitor เป็นภาษาต่างดาว
    → baud rate ต้องเป็น 115200

  ▸ ESP32 ไม่ต่อ WiFi
    → ใช้ WiFi 2.4GHz เท่านั้น (5GHz ใช้ไม่ได้)
    → เช็คตัวสะกด SSID/Password (case-sensitive)

  ▸ Dashboard "Disconnected" สีแดง
    → เช็ค SERVER_URL ต้องเป็น wss:// (มี s)
    → เปิด F12 → Console ดู error

  ▸ Vercel ไม่เห็น Dashboard
    → ไฟล์ต้องชื่อ "index.html" เท่านั้น

  ▸ Voice Command ไม่ทำงาน
    → ต้องใช้ Chrome (Safari/Firefox ไม่รองรับเต็มที่)
    → อนุญาต microphone


═══════════════════════════════════════════════════════════════════
  📋 CHECKLIST ก่อนนำไปแข่ง
═══════════════════════════════════════════════════════════════════

  Hardware:
  □ ESP32 + Sensor + Servo ทำงาน
  □ สาย USB Data 2-3 เส้น  
  □ Power Bank (10000 mAh+)
  □ มือถือ Hotspot หลัก + สำรอง

  Software:
  □ Railway uptime > 1 ชั่วโมง
  □ Dashboard เปิดได้บนมือถือ
  □ Voice Command ทำงาน

  เตรียม:
  □ QR Code พิมพ์ติดอุปกรณ์ (สร้างจาก qr-code-generator.com)
  □ Demo script ซ้อม 3 รอบขึ้นไป
  □ ทดสอบนอกบ้านสำเร็จ


═══════════════════════════════════════════════════════════════════
  📞 ลำดับการ Debug
═══════════════════════════════════════════════════════════════════

  1. เปิด Railway URL → JSON ขึ้นไหม? ถ้าไม่ → Server มีปัญหา
  2. Serial Monitor ESP32 → [WS] Connected ไหม? ถ้าไม่ → ESP32/WiFi
  3. Railway URL → "esp32":"connected"? ถ้าไม่ → ESP32 ต่อไม่ถึง
  4. Dashboard → "Server Connected"? ถ้าไม่ → SERVER_URL ผิด
  5. ทุกอย่าง connected แต่ไม่มีข้อมูล → กด F12 ดู Console


═══════════════════════════════════════════════════════════════════

  🌟 พร้อมใช้งานแล้ว ขอให้โชคดีกับการแข่งครับ!

═══════════════════════════════════════════════════════════════════
