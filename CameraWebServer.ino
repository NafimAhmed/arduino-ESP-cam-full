
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>

// ===================== AP credentials =====================
const char* ap_ssid     = "ESP32-CAM";
const char* ap_password = "12345678";

// ===================== Flash LED (AI Thinker: GPIO4) =====================
// SD card use করলে GPIO4 conflict হতে পারে।
#define FLASH_LED_PIN 4
bool flashOn = false;

// ===================== Servo pins =====================
// Try 13 & 12 first.
#define SERVO1_PIN 13
#define SERVO2_PIN 12

// Avoid camera LEDC channel 0 usage (camera uses channel/timer 0 for XCLK)
#define SERVO1_CH 4
#define SERVO2_CH 5

#define SERVO_FREQ 50
#define SERVO_RES  16
#define SERVO_PERIOD_US 20000UL

// SG90 pulse range (adjust if needed)
#define SERVO_MIN_US 500
#define SERVO_MAX_US 2400

// ===================== ESP32-CAM (AI Thinker) camera pins =====================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

WebServer server(80);

// ===================== LEDC compatibility (core 2.x + 3.x) =====================
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  #define LEDC_NEW_API 1
#else
  #define LEDC_NEW_API 0
#endif

static inline uint32_t usToDuty(uint32_t pulse_us) {
  const uint32_t maxDuty = (1UL << SERVO_RES) - 1;     // 16-bit => 65535
  return (pulse_us * maxDuty) / SERVO_PERIOD_US;       // 50Hz => 20000us period
}

void servoInit() {
#if LEDC_NEW_API
  bool ok1 = ledcAttachChannel(SERVO1_PIN, SERVO_FREQ, SERVO_RES, SERVO1_CH);
  bool ok2 = ledcAttachChannel(SERVO2_PIN, SERVO_FREQ, SERVO_RES, SERVO2_CH);
  Serial.printf("LEDC attachChannel: s1=%d s2=%d\n", ok1, ok2);
#else
  ledcSetup(SERVO1_CH, SERVO_FREQ, SERVO_RES);
  ledcSetup(SERVO2_CH, SERVO_FREQ, SERVO_RES);
  ledcAttachPin(SERVO1_PIN, SERVO1_CH);
  ledcAttachPin(SERVO2_PIN, SERVO2_CH);
  Serial.println("LEDC setup/attachPin done.");
#endif
}

void servoWriteAngle(uint8_t servoIndex, int angle) {
  angle = constrain(angle, 0, 180);
  uint32_t pulse = map(angle, 0, 180, SERVO_MIN_US, SERVO_MAX_US);
  uint32_t duty  = usToDuty(pulse);

#if LEDC_NEW_API
  if (servoIndex == 1) ledcWrite(SERVO1_PIN, duty);
  else                 ledcWrite(SERVO2_PIN, duty);
#else
  if (servoIndex == 1) ledcWrite(SERVO1_CH, duty);
  else                 ledcWrite(SERVO2_CH, duty);
#endif
}

int readAngleArg() {
  if (!server.hasArg("angle")) return -1;
  int a = server.arg("angle").toInt();
  return constrain(a, 0, 180);
}

// ===================== Camera helpers =====================
framesize_t parseFrameSize(String fs) {
  fs.toUpperCase();
  if (fs == "QQVGA") return FRAMESIZE_QQVGA;
  if (fs == "QVGA")  return FRAMESIZE_QVGA;
  if (fs == "VGA")   return FRAMESIZE_VGA;
  if (fs == "SVGA")  return FRAMESIZE_SVGA;
  if (fs == "XGA")   return FRAMESIZE_XGA;
  if (fs == "SXGA")  return FRAMESIZE_SXGA;
  if (fs == "UXGA")  return FRAMESIZE_UXGA;
  return FRAMESIZE_QVGA;
}

// ===================== Web UI =====================
// /jpg pseudo-stream + Zoom slider (CSS scale)
// Recording: Browser side MediaRecorder (canvas capture) -> WebM file
void handleRoot() {
  String html = R"rawliteral(
  <html>
    <head>
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <title>ESP32-CAM + 2 Servos</title>
      <style>
        body{font-family:Arial;text-align:center;margin:0;padding:12px;}
        .box{max-width:820px;margin:0 auto;}
        .camWrap{max-width:720px;margin:0 auto;border-radius:12px;overflow:hidden;border:1px solid #ddd;}
        img{width:100%;display:block;transform-origin:center center;}
        .card{margin-top:12px;padding:12px;border:1px solid #ddd;border-radius:12px;}
        input[type=range]{width:100%;}
        small{color:#666;}
        button{padding:10px 12px;border-radius:10px;border:1px solid #ccc;background:#fff;cursor:pointer;}
        button:active{transform:scale(0.98);}
        .row{display:flex;gap:8px;flex-wrap:wrap;justify-content:center;align-items:center;}
        select{padding:10px 12px;border-radius:10px;border:1px solid #ccc;}
        .pill{padding:6px 10px;border-radius:999px;border:1px solid #ddd;display:inline-block;}
      </style>
    </head>
    <body>
      <div class="box">
        <h2>ESP32-CAM Live</h2>

        <div class="camWrap">
          <img id="cam" src="/jpg" />
        </div>

        <div style="margin-top:10px;">
          <small>Tip: Zoom slider / Flash / Snap / Record সব কাজ করবে streaming চলাকালীন</small>
        </div>

        <div class="card">
          <div class="row" style="margin-bottom:10px; align-items:flex-start;">
            <!-- ✅ ZOOM SLIDER -->
            <div style="flex:1; min-width:260px; text-align:left;">
              <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:6px;">
                <b>Zoom</b>
                <span class="pill"><b id="zv">1.0x</b></span>
              </div>
              <input id="zoomSlider" type="range" min="1" max="3" value="1" step="0.1"
                     oninput="setZoom(this.value)">
            </div>

            <!-- Controls -->
            <button id="flashBtn" onclick="toggleFlash()">Flash: OFF</button>
            <button onclick="snap()">Snap</button>
            <button id="recBtn" onclick="toggleRec()">● Record</button>
          </div>

          <div class="row">
            <select onchange="setFrameSize(this.value)">
              <option value="QVGA">QVGA (fast)</option>
              <option value="VGA" selected>VGA (detail)</option>
              <option value="SVGA">SVGA (slow)</option>
              <option value="UXGA">UXGA (very slow)</option>
            </select>

            <select onchange="setQuality(this.value)">
              <option value="16">Quality: 16 (normal)</option>
              <option value="12" selected>Quality: 12 (better)</option>
              <option value="10">Quality: 10 (best)</option>
              <option value="20">Quality: 20 (faster)</option>
            </select>
          </div>
        </div>

        <div class="card">
          <h3>Servo 1: <span id="v1">90</span>&deg;</h3>
          <input type="range" min="0" max="180" value="90"
                 oninput="setServo(1,this.value)">
        </div>

        <div class="card">
          <h3>Servo 2: <span id="v2">90</span>&deg;</h3>
          <input type="range" min="0" max="180" value="90"
                 oninput="setServo(2,this.value)">
        </div>

        <!-- hidden canvas for recording -->
        <canvas id="recCanvas" style="display:none;"></canvas>
      </div>

      <script>
        const cam = document.getElementById('cam');
        const canvas = document.getElementById('recCanvas');
        const ctx = canvas.getContext('2d');

        let zoom = 1.0;
        let flash = 0;

        // pseudo-stream refresh
        function refreshCam(){ cam.src = '/jpg?ts=' + Date.now(); }
        setInterval(refreshCam, 150);

        cam.onload = () => {
          // keep canvas updated for recording
          const w = cam.naturalWidth || 640;
          const h = cam.naturalHeight || 480;
          if (canvas.width !== w || canvas.height !== h) {
            canvas.width = w;
            canvas.height = h;
          }
          try { ctx.drawImage(cam, 0, 0, canvas.width, canvas.height); } catch(e){}
        };

        // ✅ Zoom slider
        function applyZoom(){
          cam.style.transform = `scale(${zoom})`;
          document.getElementById('zv').innerText = zoom.toFixed(1) + 'x';
          const zs = document.getElementById('zoomSlider');
          if (zs && parseFloat(zs.value) !== zoom) zs.value = zoom.toFixed(1);
        }
        function setZoom(val){
          zoom = Math.min(3.0, Math.max(1.0, parseFloat(val)));
          applyZoom();
        }

        function setServo(n, val){
          document.getElementById('v'+n).innerText = val;
          fetch('/servo' + n + '?angle=' + val).catch(()=>{});
        }

        function toggleFlash(){
          flash = flash ? 0 : 1;
          fetch('/flash?val=' + flash).catch(()=>{});
          document.getElementById('flashBtn').innerText = 'Flash: ' + (flash ? 'ON' : 'OFF');
        }

        function setFrameSize(fs){ fetch('/cam?fs=' + encodeURIComponent(fs)).catch(()=>{}); }
        function setQuality(q){ fetch('/cam?q=' + encodeURIComponent(q)).catch(()=>{}); }

        // Snap: download original JPEG from ESP32
        async function snap(){
          try{
            const res = await fetch('/snap?ts=' + Date.now());
            const blob = await res.blob();
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = 'capture_' + Date.now() + '.jpg';
            document.body.appendChild(a);
            a.click();
            a.remove();
            URL.revokeObjectURL(url);
          }catch(e){}
        }

        // Recording: Browser side WebM using canvas captureStream
        let rec = false;
        let mediaRecorder = null;
        let chunks = [];

        function toggleRec(){ (!rec) ? startRec() : stopRec(); }

        function startRec(){
          chunks = [];
          let stream = null;
          try { stream = canvas.captureStream(10); } catch(e) {
            alert('Your browser does not support canvas captureStream.');
            return;
          }

          try { mediaRecorder = new MediaRecorder(stream, { mimeType: 'video/webm;codecs=vp8' }); }
          catch(e) { mediaRecorder = new MediaRecorder(stream); }

          mediaRecorder.ondataavailable = (e) => { if(e.data && e.data.size) chunks.push(e.data); };
          mediaRecorder.onstop = () => {
            const blob = new Blob(chunks, { type: 'video/webm' });
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = 'record_' + Date.now() + '.webm';
            document.body.appendChild(a);
            a.click();
            a.remove();
            URL.revokeObjectURL(url);
            chunks = [];
          };

          mediaRecorder.start();
          rec = true;
          document.getElementById('recBtn').innerText = '■ Stop';
        }

        function stopRec(){
          if(mediaRecorder && rec) mediaRecorder.stop();
          rec = false;
          document.getElementById('recBtn').innerText = '● Record';
        }

        applyZoom();
      </script>
    </body>
  </html>
  )rawliteral";

  server.send(200, "text/html", html);
}

// ===================== Camera JPG endpoint =====================
void sendJpegFrame(bool asDownload) {
  WiFiClient client = server.client();
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "Camera capture failed");
    return;
  }

  if (asDownload) {
    server.sendHeader("Content-Disposition", "attachment; filename=capture.jpg");
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  } else {
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  }

  server.setContentLength(fb->len);
  server.send(200, "image/jpeg", "");
  client.write(fb->buf, fb->len);

  esp_camera_fb_return(fb);
}

void handleJPG()  { sendJpegFrame(false); }
void handleSNAP() { sendJpegFrame(true); }

// ===================== Flash endpoint =====================
void handleFlash() {
  int v = 0;
  if (server.hasArg("val")) v = server.arg("val").toInt();
  v = v ? 1 : 0;
  flashOn = (v == 1);
  digitalWrite(FLASH_LED_PIN, flashOn ? HIGH : LOW);
  server.send(200, "text/plain", flashOn ? "ON" : "OFF");
}

// ===================== Camera control endpoint =====================
// /cam?fs=VGA
// /cam?q=12
// /cam?bright=1
void handleCam() {
  sensor_t * s = esp_camera_sensor_get();
  bool changed = false;

  if (server.hasArg("fs")) {
    framesize_t fs = parseFrameSize(server.arg("fs"));
    if (s->set_framesize) s->set_framesize(s, fs);
    changed = true;
  }
  if (server.hasArg("q")) {
    int q = server.arg("q").toInt();
    q = constrain(q, 10, 63); // lower = better quality
    if (s->set_quality) s->set_quality(s, q);
    changed = true;
  }
  if (server.hasArg("bright")) {
    int b = server.arg("bright").toInt();
    b = constrain(b, -2, 2);
    if (s->set_brightness) s->set_brightness(s, b);
    changed = true;
  }

  server.send(200, "text/plain", changed ? "OK" : "No params");
}

// ===================== Servo endpoints =====================
void handleServo1() {
  int angle = readAngleArg();
  if (angle < 0) {
    server.send(400, "text/plain", "Missing angle. Use ?angle=0..180");
    return;
  }
  servoWriteAngle(1, angle);
  server.send(200, "text/plain", "OK Servo1=" + String(angle));
}

void handleServo2() {
  int angle = readAngleArg();
  if (angle < 0) {
    server.send(400, "text/plain", "Missing angle. Use ?angle=0..180");
    return;
  }
  servoWriteAngle(2, angle);
  server.send(200, "text/plain", "OK Servo2=" + String(angle));
}

// ===================== Setup =====================
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  // Flash init
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  // 1) Servo init first
  servoInit();
  servoWriteAngle(1, 90);
  servoWriteAngle(2, 90);

  // 2) Camera init
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count     = 2;
  } else {
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed 0x%x\n", err);
    return;
  }

  // 3) WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  IPAddress ip = WiFi.softAPIP();

  Serial.print("AP IP address: ");
  Serial.println(ip);
  Serial.print("Open in phone: http://");
  Serial.println(ip);

  // 4) Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/jpg", HTTP_GET, handleJPG);
  server.on("/snap", HTTP_GET, handleSNAP);

  server.on("/flash", HTTP_GET, handleFlash);
  server.on("/cam", HTTP_GET, handleCam);

  server.on("/servo1", HTTP_GET, handleServo1);
  server.on("/servo2", HTTP_GET, handleServo2);

  server.begin();
  Serial.println("Web server started.");
}

void loop() {
  server.handleClient();
  delay(2);
}










