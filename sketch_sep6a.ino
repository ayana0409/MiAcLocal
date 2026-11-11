#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

// ==========================================
// 1. HARDWARE PIN CONFIGURATION (ESP32-S3)
// ==========================================
#define PIN_ONEWIRE     21  // DS18B20 OneWire DQ pin
#define PIN_IR_RECV     14  // IR Receiver OUT pin (VS1838B)
#define PIN_IR_SEND     47  // IR Transmitter LED pin

WebServer server(80);
Preferences prefs;
OneWire oneWire(PIN_ONEWIRE);
DallasTemperature sensors(&oneWire);

// Buffer size: 1024 uint16 elements for long AC frames
IRrecv irrecv(PIN_IR_RECV, 1024, 50, true);
IRsend irsend(PIN_IR_SEND);

// ==========================================
// 2. SYSTEM STATE & TEMPERATURE CONFIG
// ==========================================
float currentTemp = -999.0f;
float targetTemp = 16.0f;     // Default on startup: 16.0 C
float tempGap = 3.0f;         // Default on startup: 3.0 C
float fanOffset = 1.0f;       // Offset below target for Fan threshold (e.g., Target - 1.0 C)

bool isSystemOn = false;      // Default on startup: OFF (Standby)
bool tempSensorReady = false;

enum RunningMode { MODE_IS_COOL, MODE_IS_FAN };
RunningMode currentACMode = MODE_IS_COOL; // Default mode: COOL

// Compressor protection interval (3 minutes = 180,000 ms)
unsigned long lastSwitchTime = 0;
const unsigned long MIN_COMPRESSOR_INTERVAL = 180000;

// Maximum pulse count per IR packet (supports Daikin, Panasonic, Mitsubishi, and NEC)
#define MAX_RAW 750
#define MIN_AC_RAW_PULSES 20 // Filter out noise glitches (< 20 pulses)

// Buffers for storing the 4 core buttons
uint16_t rawMode[MAX_RAW];
uint16_t rawPower[MAX_RAW];
uint16_t rawUp[MAX_RAW];
uint16_t rawDown[MAX_RAW];

size_t lenMode = 0;
size_t lenPower = 0;
size_t lenUp = 0;
size_t lenDown = 0;

// Self-reflection prevention timestamp
unsigned long ignoreIRUntil = 0;

// Learning timeout management
unsigned long learningTimeout = 0;
const unsigned long LEARNING_WINDOW = 20000; // 20 seconds window to press remote

String irLog = "System booted in standby mode (OFF, Target: 16.0 C, Gap: 3.0 C, Mode: COOL).";
String targetAssign = "none";

// ==========================================
// 3. HELPER FUNCTIONS
// ==========================================
float getFanThreshold() {
  return targetTemp - fanOffset; // e.g. 16.0 - 1.0 = 15.0 C
}

float getCoolThreshold() {
  return getFanThreshold() + tempGap; // e.g. 15.0 + 3.0 = 18.0 C
}

String escapeJson(const String& s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

void saveRaw(const char* kLen, const char* kData, const uint16_t* buf, size_t len) {
  prefs.begin("ac_ir", false);
  prefs.putUInt(kLen, len);
  prefs.putBytes(kData, buf, len * sizeof(uint16_t));
  prefs.end();
}

size_t loadRaw(const char* kLen, const char* kData, uint16_t* buf) {
  prefs.begin("ac_ir", true);
  size_t len = prefs.getUInt(kLen, 0);
  if (len >= MIN_AC_RAW_PULSES && len <= MAX_RAW) {
    prefs.getBytes(kData, buf, len * sizeof(uint16_t));
  } else {
    len = 0;
  }
  prefs.end();
  return len;
}

// Emit raw IR pulse stream with loopback suppression
void emitSingleRaw(const uint16_t* buf, size_t len) {
  if (len == 0 || buf == nullptr) return;
  // Ignore receiver for 1 second to avoid picking up our own transmitted burst
  ignoreIRUntil = millis() + 1000;
  irsend.sendRaw(buf, len, 38);
  delay(60);
  irrecv.resume();
}

// Precise pulse comparator: Counts how many pulses differ by more than jitter threshold (350us)
int countMismatchedPulses(const uint16_t* curRaw, size_t curLen, const uint16_t* savedRaw, size_t savedLen) {
  if (savedLen < MIN_AC_RAW_PULSES || curLen < MIN_AC_RAW_PULSES) return 9999;
  if (abs((int)curLen - (int)savedLen) > 4) return 9999;

  size_t checkLen = (curLen < savedLen) ? curLen : savedLen;
  int mismatches = 0;

  for (size_t i = 0; i < checkLen; i++) {
    int diff = abs((int)curRaw[i] - (int)savedRaw[i]);
    if (diff > 350) {
      mismatches++;
    }
  }
  return mismatches;
}

// Switch from Cool to Fan by pressing MODE button 2 times (Cool -> Dry -> Fan)
void switchToFan() {
  if (lenMode == 0) return;
  emitSingleRaw(rawMode, lenMode);
  delay(850); // Wait 850ms for AC unit to register 1st beep
  emitSingleRaw(rawMode, lenMode);
}

// Switch from Fan to Cool by pressing MODE button 1 time (Fan -> Cool)
void switchToCool() {
  if (lenMode == 0) return;
  emitSingleRaw(rawMode, lenMode);
}

// ==========================================
// 4. EMBEDDED WEB INTERFACE
// ==========================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Smart AC Thermostat</title>
  <style>
    :root {
      --bg: #0f172a;
      --card-bg: #1e293b;
      --border: #334155;
      --text: #f8fafc;
      --muted: #94a3b8;
      --primary: #3b82f6;
      --primary-hover: #2563eb;
      --cool: #06b6d4;
      --fan: #10b981;
      --warn: #f59e0b;
      --danger: #ef4444;
    }
    * { box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      background: var(--bg);
      color: var(--text);
      padding: 16px;
      margin: 0;
      display: flex;
      justify-content: center;
    }
    .container { width: 100%; max-width: 520px; }
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: 16px;
    }
    h1 { font-size: 1.25rem; font-weight: 700; margin: 0; }
    .badge {
      font-size: 0.75rem;
      padding: 4px 8px;
      border-radius: 9999px;
      font-weight: 600;
      text-transform: uppercase;
    }
    .badge-on { background: rgba(16, 185, 129, 0.2); color: #34d399; }
    .badge-off { background: rgba(239, 68, 68, 0.2); color: #f87171; }
    .card {
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 16px;
      margin-bottom: 16px;
      box-shadow: 0 4px 6px -1px rgba(0,0,0,0.2);
    }
    .temp-grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 12px;
      margin-bottom: 14px;
    }
    .temp-box {
      background: rgba(15, 23, 42, 0.6);
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 12px;
      text-align: center;
    }
    .temp-label { font-size: 0.8rem; color: var(--muted); margin-bottom: 4px; }
    .temp-val { font-size: 1.65rem; font-weight: 700; color: #38bdf8; }
    .target-val { color: var(--warn); }
    .btn-row { display: flex; gap: 8px; align-items: center; justify-content: center; }
    button {
      background: var(--primary);
      color: #fff;
      border: none;
      padding: 7px 12px;
      border-radius: 6px;
      font-weight: 600;
      font-size: 0.85rem;
      cursor: pointer;
      transition: background 0.15s;
    }
    button:hover { background: var(--primary-hover); }
    button.btn-danger { background: var(--danger); }
    button.btn-danger:hover { background: #dc2626; }
    button.btn-outline {
      background: transparent;
      border: 1px solid var(--border);
      color: var(--text);
    }
    button.btn-outline:hover { background: rgba(255,255,255,0.05); }
    button.btn-active {
      background: var(--warn);
      color: #000;
      animation: pulse 1.2s infinite;
    }
    @keyframes pulse {
      0% { opacity: 1; }
      50% { opacity: 0.6; }
      100% { opacity: 1; }
    }
    .row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 8px 0;
      border-bottom: 1px solid rgba(51, 65, 85, 0.4);
      font-size: 0.9rem;
    }
    .row:last-child { border-bottom: none; }
    .status-text { font-weight: 600; }
    .mode-cool { color: var(--cool); }
    .mode-fan { color: var(--fan); }
    .learn-grid {
      display: grid;
      grid-template-columns: 1fr auto auto auto;
      gap: 6px;
      align-items: center;
      margin-bottom: 10px;
    }
    .learn-name { font-size: 0.9rem; font-weight: 500; }
    .tag { font-size: 0.75rem; padding: 2px 6px; border-radius: 4px; display: inline-block; margin-top: 3px; }
    .tag-yes { background: rgba(16, 185, 129, 0.2); color: #34d399; }
    .tag-no { background: rgba(245, 158, 11, 0.2); color: #fbbf24; }
    #logBox {
      background: #090d16;
      border: 1px solid var(--border);
      color: #10b981;
      padding: 10px;
      border-radius: 6px;
      font-family: monospace;
      font-size: 0.8rem;
      height: 95px;
      overflow-y: auto;
      white-space: pre-wrap;
      word-break: break-all;
    }
    .banner {
      background: rgba(245, 158, 11, 0.15);
      border: 1px solid var(--warn);
      border-radius: 8px;
      padding: 10px;
      margin-bottom: 12px;
      font-size: 0.85rem;
      display: none;
    }
    .gap-preview {
      background: rgba(15, 23, 42, 0.5);
      border: 1px dashed var(--border);
      border-radius: 8px;
      padding: 10px 14px;
      margin-top: 10px;
      font-size: 0.85rem;
      line-height: 1.5;
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <h1>AC Thermostat</h1>
      <span id="pwrBadge" class="badge badge-off">Standby</span>
    </div>

    <!-- Active Learning Banner -->
    <div id="learningBanner" class="banner">
      <strong>LEARNING IN PROGRESS:</strong>
      <div id="learningTargetText" style="margin-top: 4px;">Point remote at ESP32 and press button...</div>
      <button onclick="setAssign('none')" class="btn-outline" style="margin-top: 8px; font-size: 0.75rem; padding: 4px 8px;">Cancel Learning</button>
    </div>

    <!-- Temperature & Hysteresis Card -->
    <div class="card">
      <div class="temp-grid">
        <div class="temp-box">
          <div class="temp-label">Room Temp (DS18B20)</div>
          <div id="roomTemp" class="temp-val">--.- &deg;C</div>
        </div>
        <div class="temp-box">
          <div class="temp-label">Target Temperature</div>
          <div id="targetTemp" class="temp-val target-val">16.0 &deg;C</div>
          <div class="btn-row" style="margin-top: 6px;">
            <button onclick="changeTarget(-1)">-1&deg;</button>
            <button onclick="changeTarget(1)">+1&deg;</button>
          </div>
        </div>
      </div>

      <!-- Temperature Gap (Hysteresis) Configuration -->
      <div class="row">
        <span>Temp Gap (Khoảng cách):</span>
        <div class="btn-row">
          <button onclick="changeGap(-0.5)" class="btn-outline">-0.5&deg;</button>
          <span id="gapVal" style="font-weight: 700; color: #38bdf8; min-width: 48px; text-align: center;">3.0 &deg;C</span>
          <button onclick="changeGap(0.5)" class="btn-outline">+0.5&deg;</button>
        </div>
      </div>

      <!-- Dynamic Trigger Preview -->
      <div class="gap-preview">
        <div>&bull; <strong style="color: var(--cool);">COOL Trigger:</strong> Temp &ge; <span id="coolThresh" style="font-weight: bold;">18.0</span> &deg;C</div>
        <div>&bull; <strong style="color: var(--fan);">FAN Trigger:</strong> Temp &le; <span id="fanThresh" style="font-weight: bold;">15.0</span> &deg;C</div>
        <div style="color: var(--muted); font-size: 0.75rem; margin-top: 4px;">Total span between thresholds: <span id="spanPreview">3.0</span> &deg;C</div>
      </div>

      <div class="row" style="margin-top: 10px;">
        <span>System Control:</span>
        <button id="pwrBtn" onclick="togglePower()" class="btn-outline">Turn ON</button>
      </div>
      <div class="row">
        <span>Thermostat State:</span>
        <span id="acState" class="status-text">Paused (System Off)</span>
      </div>
      <div class="row">
        <span>Compressor Lock:</span>
        <span id="lockState" style="color: var(--muted); font-size: 0.85rem;">Checking...</span>
      </div>
    </div>

    <!-- IR Remote Buttons Card -->
    <div class="card">
      <h2 style="font-size: 1rem; margin: 0 0 12px 0;">Learned Remote Buttons</h2>
      
      <!-- Mode Button -->
      <div class="learn-grid">
        <div>
          <div class="learn-name">1. MODE (Cool-Dry-Fan)</div>
          <span id="tagMode" class="tag tag-no">Not Learned</span>
        </div>
        <button id="btnLearnMode" onclick="setAssign('mode')" class="btn-outline">Learn</button>
        <button onclick="testEmit('mode_1')" title="Press 1 time to switch to Cool">Test x1</button>
        <button onclick="testEmit('mode_2')" title="Press 2 times to switch to Fan">Test x2</button>
      </div>

      <!-- Power Button -->
      <div class="learn-grid" style="grid-template-columns: 1fr auto auto;">
        <div>
          <div class="learn-name">2. POWER (On/Off)</div>
          <span id="tagPower" class="tag tag-no">Not Learned</span>
        </div>
        <button id="btnLearnPower" onclick="setAssign('pwr')" class="btn-outline">Learn</button>
        <button onclick="testEmit('pwr')">Test</button>
      </div>

      <!-- Temp Up Button -->
      <div class="learn-grid" style="grid-template-columns: 1fr auto auto;">
        <div>
          <div class="learn-name">3. TEMP UP (+)</div>
          <span id="tagUp" class="tag tag-no">Not Learned</span>
        </div>
        <button id="btnLearnUp" onclick="setAssign('up')" class="btn-outline">Learn</button>
        <button onclick="testEmit('up')">Test</button>
      </div>

      <!-- Temp Down Button -->
      <div class="learn-grid" style="grid-template-columns: 1fr auto auto;">
        <div>
          <div class="learn-name">4. TEMP DOWN (-)</div>
          <span id="tagDown" class="tag tag-no">Not Learned</span>
        </div>
        <button id="btnLearnDown" onclick="setAssign('down')" class="btn-outline">Learn</button>
        <button onclick="testEmit('down')">Test</button>
      </div>
    </div>

    <!-- Logs Card -->
    <div class="card">
      <div style="font-size: 0.85rem; color: var(--muted); margin-bottom: 6px;">Event & IR Log:</div>
      <div id="logBox">Connecting to device...</div>
    </div>
  </div>

  <script>
    function updateStatus() {
      fetch('/api/status')
        .then(res => res.json())
        .then(data => {
          document.getElementById('roomTemp').innerHTML = (data.sensor_ready ? data.temp : '--.-') + ' &deg;C';
          document.getElementById('targetTemp').innerHTML = data.target + ' &deg;C';
          document.getElementById('gapVal').innerHTML = data.gap + ' &deg;C';
          document.getElementById('coolThresh').innerText = data.cool_thresh;
          document.getElementById('fanThresh').innerText = data.fan_thresh;
          document.getElementById('spanPreview').innerText = data.gap;

          const pwrBadge = document.getElementById('pwrBadge');
          const pwrBtn = document.getElementById('pwrBtn');
          if (data.is_on) {
            pwrBadge.innerText = 'ENABLED';
            pwrBadge.className = 'badge badge-on';
            pwrBtn.innerText = 'Turn OFF';
            pwrBtn.className = 'btn-danger';
          } else {
            pwrBadge.innerText = 'STANDBY (OFF)';
            pwrBadge.className = 'badge badge-off';
            pwrBtn.innerText = 'Turn ON';
            pwrBtn.className = 'btn-outline';
          }

          const stateSpan = document.getElementById('acState');
          if (!data.is_on) {
            stateSpan.innerText = 'Paused (System Off)';
            stateSpan.className = 'status-text';
          } else if (data.mode === 'COOL') {
            stateSpan.innerText = 'Cooling (Compressor Active)';
            stateSpan.className = 'status-text mode-cool';
          } else {
            stateSpan.innerText = 'Fan Only (Compressor Rest)';
            stateSpan.className = 'status-text mode-fan';
          }

          const lockSpan = document.getElementById('lockState');
          if (data.compressor_rem > 0) {
            lockSpan.innerText = 'Locked (' + data.compressor_rem + 's remaining)';
            lockSpan.style.color = 'var(--warn)';
          } else {
            lockSpan.innerText = 'Ready to switch';
            lockSpan.style.color = 'var(--fan)';
          }

          updateTag('tagMode', data.len_mode);
          updateTag('tagPower', data.len_power);
          updateTag('tagUp', data.len_up);
          updateTag('tagDown', data.len_down);

          // Update active learning banner
          const banner = document.getElementById('learningBanner');
          const bannerText = document.getElementById('learningTargetText');
          const btnMode = document.getElementById('btnLearnMode');
          const btnPower = document.getElementById('btnLearnPower');
          const btnUp = document.getElementById('btnLearnUp');
          const btnDown = document.getElementById('btnLearnDown');

          btnMode.className = 'btn-outline';
          btnPower.className = 'btn-outline';
          btnUp.className = 'btn-outline';
          btnDown.className = 'btn-outline';

          if (data.assign !== 'none') {
            banner.style.display = 'block';
            bannerText.innerText = 'Waiting for ' + data.assign.toUpperCase() + ' button (' + data.learn_rem + 's remaining)...';
            if (data.assign === 'mode') btnMode.className = 'btn-active';
            if (data.assign === 'pwr') btnPower.className = 'btn-active';
            if (data.assign === 'up') btnUp.className = 'btn-active';
            if (data.assign === 'down') btnDown.className = 'btn-active';
          } else {
            banner.style.display = 'none';
          }

          if (data.log) {
            const logBox = document.getElementById('logBox');
            logBox.innerText = data.log;
          }
        })
        .catch(err => {
          document.getElementById('logBox').innerText = 'Connection lost. Reconnecting...';
        });
    }

    function updateTag(id, len) {
      const el = document.getElementById(id);
      if (len > 0) {
        el.innerText = 'Learned (' + len + ' pulses)';
        el.className = 'tag tag-yes';
      } else {
        el.innerText = 'Not Learned';
        el.className = 'tag tag-no';
      }
    }

    function togglePower() {
      fetch('/api/toggle_power').then(() => updateStatus());
    }

    function changeTarget(diff) {
      fetch('/api/set_target?val=' + diff).then(() => updateStatus());
    }

    function changeGap(diff) {
      fetch('/api/set_gap?val=' + diff).then(() => updateStatus());
    }

    function setAssign(target) {
      fetch('/api/set_assign?target=' + target).then(() => updateStatus());
    }

    function testEmit(cmd) {
      fetch('/api/test_emit?cmd=' + cmd).then(() => updateStatus());
    }

    setInterval(updateStatus, 1500);
    updateStatus();
  </script>
</body>
</html>
)rawliteral";

// ==========================================
// 5. SETUP SYSTEM
// ==========================================
void setup() {
  Serial.begin(115200);

  // Initialize Temperature Sensor
  sensors.begin();
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();

  // Initialize IR Transmitter and Receiver
  irsend.begin();
  irrecv.enableIRIn();

  // Load IR profiles from Flash memory
  lenMode  = loadRaw("l_mod", "d_mod", rawMode);
  lenPower = loadRaw("l_pwr", "d_pwr", rawPower);
  lenUp    = loadRaw("l_up",  "d_up",  rawUp);
  lenDown  = loadRaw("l_dn",  "d_dn",  rawDown);

  // Boot defaults specified by user: OFF, Target 16.0 C, Gap 3.0 C, Mode COOL
  isSystemOn = false;
  targetTemp = 16.0f;
  tempGap    = 3.0f;
  fanOffset  = 1.0f;
  currentACMode = MODE_IS_COOL;

  // Persist the default settings to Flash memory
  prefs.begin("ac_ir", false);
  prefs.putFloat("tgt", targetTemp);
  prefs.putFloat("gap", tempGap);
  prefs.putFloat("foff", fanOffset);
  prefs.end();

  // Initialize Wi-Fi Access Point
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32_MAYLANH", "12345678");

  // Web routes
  server.on("/", []() {
    server.send(200, "text/html", INDEX_HTML);
  });

  server.on("/api/status", []() {
    unsigned long now = millis();
    unsigned long elapsed = now - lastSwitchTime;
    long remSec = 0;
    if (elapsed < MIN_COMPRESSOR_INTERVAL) {
      remSec = (long)((MIN_COMPRESSOR_INTERVAL - elapsed) / 1000);
    }

    long learnRem = 0;
    if (targetAssign != "none" && learningTimeout > now) {
      learnRem = (long)((learningTimeout - now) / 1000);
    }

    float fanThresh = getFanThreshold();
    float coolThresh = getCoolThreshold();

    String json = "{";
    json += "\"temp\":\"" + String(currentTemp, 1) + "\",";
    json += "\"target\":\"" + String(targetTemp, 1) + "\",";
    json += "\"gap\":\"" + String(tempGap, 1) + "\",";
    json += "\"fan_thresh\":\"" + String(fanThresh, 1) + "\",";
    json += "\"cool_thresh\":\"" + String(coolThresh, 1) + "\",";
    json += "\"is_on\":" + String(isSystemOn ? "true" : "false") + ",";
    json += "\"sensor_ready\":" + String(tempSensorReady ? "true" : "false") + ",";
    json += "\"mode\":\"" + String(currentACMode == MODE_IS_COOL ? "COOL" : "FAN") + "\",";
    json += "\"compressor_rem\":" + String(remSec) + ",";
    json += "\"learn_rem\":" + String(learnRem) + ",";
    json += "\"len_mode\":" + String(lenMode) + ",";
    json += "\"len_power\":" + String(lenPower) + ",";
    json += "\"len_up\":" + String(lenUp) + ",";
    json += "\"len_down\":" + String(lenDown) + ",";
    json += "\"assign\":\"" + targetAssign + "\",";
    json += "\"log\":\"" + escapeJson(irLog) + "\"";
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/api/toggle_power", []() {
    isSystemOn = !isSystemOn;
    if (lenPower > 0) emitSingleRaw(rawPower, lenPower);
    irLog = "Power toggled via Web UI (" + String(isSystemOn ? "ON" : "OFF") + ").";
    server.send(200, "text/plain", "OK");
  });

  server.on("/api/set_target", []() {
    if (server.hasArg("val")) {
      float diff = server.arg("val").toFloat();
      targetTemp += diff;
      if (targetTemp > 32.0f) targetTemp = 32.0f;
      if (targetTemp < 16.0f) targetTemp = 16.0f;

      prefs.begin("ac_ir", false);
      prefs.putFloat("tgt", targetTemp);
      prefs.end();
      irLog = "Target: " + String(targetTemp, 1) + " C (Cool >= " + String(getCoolThreshold(), 1) + " C, Fan <= " + String(getFanThreshold(), 1) + " C).";
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/api/set_gap", []() {
    if (server.hasArg("val")) {
      float diff = server.arg("val").toFloat();
      tempGap += diff;
      if (tempGap < 1.0f) tempGap = 1.0f;
      if (tempGap > 6.0f) tempGap = 6.0f;

      prefs.begin("ac_ir", false);
      prefs.putFloat("gap", tempGap);
      prefs.end();
      irLog = "Gap updated: " + String(tempGap, 1) + " C (Cool >= " + String(getCoolThreshold(), 1) + " C, Fan <= " + String(getFanThreshold(), 1) + " C).";
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/api/set_assign", []() {
    if (server.hasArg("target")) {
      String target = server.arg("target");
      if (target == "mode" || target == "pwr" || target == "up" || target == "down") {
        targetAssign = target;
        learningTimeout = millis() + LEARNING_WINDOW;
        irLog = "Learning [" + targetAssign + "]: Aim remote at ESP32 and press the button within 20s.";
      } else {
        targetAssign = "none";
        learningTimeout = 0;
        irLog = "Learning cancelled.";
      }
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/api/test_emit", []() {
    if (server.hasArg("cmd")) {
      String cmd = server.arg("cmd");
      if (cmd == "mode_1") {
        if (lenMode > 0) {
          switchToCool();
          irLog = "Emitted MODE x1 (Switch to Cool).";
        } else {
          irLog = "MODE button not learned yet.";
        }
      } else if (cmd == "mode_2") {
        if (lenMode > 0) {
          switchToFan();
          irLog = "Emitted MODE x2 (Switch to Fan).";
        } else {
          irLog = "MODE button not learned yet.";
        }
      } else if (cmd == "pwr") {
        if (lenPower > 0) {
          emitSingleRaw(rawPower, lenPower);
          irLog = "Emitted POWER toggle.";
        } else {
          irLog = "POWER button not learned yet.";
        }
      } else if (cmd == "up") {
        if (lenUp > 0) {
          emitSingleRaw(rawUp, lenUp);
          irLog = "Emitted TEMP UP (+).";
        } else {
          irLog = "TEMP UP button not learned yet.";
        }
      } else if (cmd == "down") {
        if (lenDown > 0) {
          emitSingleRaw(rawDown, lenDown);
          irLog = "Emitted TEMP DOWN (-).";
        } else {
          irLog = "TEMP DOWN button not learned yet.";
        }
      }
    }
    server.send(200, "text/plain", "OK");
  });

  server.begin();
  lastSwitchTime = millis();
}

// ==========================================
// 6. MAIN RUNTIME LOOP
// ==========================================
void loop() {
  server.handleClient();

  // Learning timeout check (auto cancel after 20s if no button pressed)
  if (targetAssign != "none" && millis() > learningTimeout) {
    targetAssign = "none";
    irLog = "Learning timed out. No valid signal received.";
  }

  // Asynchronous temperature acquisition every 2 seconds
  static unsigned long lastTempRead = 0;
  if (millis() - lastTempRead > 2000) {
    float t = sensors.getTempCByIndex(0);
    // Filter out disconnected sensor values (-127.0 C)
    if (t > -40.0f && t < 80.0f) {
      currentTemp = t;
      tempSensorReady = true;
    } else {
      tempSensorReady = false;
    }
    sensors.requestTemperatures();
    lastTempRead = millis();
  }

  // IR Signal Handling
  decode_results results;
  if (irrecv.decode(&results)) {
    size_t count = getCorrectedRawLength(&results);
    uint16_t* rawArray = resultToRawArray(&results);
    String protocolName = typeToString(results.decode_type);

    // Filter out self-reflection from our own IR transmitter
    if (millis() >= ignoreIRUntil) {

      // --- LEARNING MODE ---
      if (targetAssign != "none") {
        // Reject short noise glitches (< 20 pulses)
        if (count < MIN_AC_RAW_PULSES) {
          irLog = "Ignored noise pulse (" + String(count) + " pulses < " + String(MIN_AC_RAW_PULSES) + "). Keep aiming remote...";
        } else if (count > MAX_RAW) {
          irLog = "Signal exceeded buffer (" + String(count) + " > " + String(MAX_RAW) + ").";
        } else {
          // Valid remote button received
          if (targetAssign == "mode") {
            memcpy(rawMode, rawArray, count * sizeof(uint16_t));
            lenMode = count;
            saveRaw("l_mod", "d_mod", rawMode, lenMode);
            irLog = "Learned MODE button: " + protocolName + " (" + String(count) + " pulses).";
            targetAssign = "none";
          } else if (targetAssign == "pwr") {
            memcpy(rawPower, rawArray, count * sizeof(uint16_t));
            lenPower = count;
            saveRaw("l_pwr", "d_pwr", rawPower, lenPower);
            irLog = "Learned POWER button: " + protocolName + " (" + String(count) + " pulses).";
            targetAssign = "none";
          } else if (targetAssign == "up") {
            memcpy(rawUp, rawArray, count * sizeof(uint16_t));
            lenUp = count;
            saveRaw("l_up", "d_up", rawUp, lenUp);
            irLog = "Learned TEMP UP button: " + protocolName + " (" + String(count) + " pulses).";
            targetAssign = "none";
          } else if (targetAssign == "down") {
            memcpy(rawDown, rawArray, count * sizeof(uint16_t));
            lenDown = count;
            saveRaw("l_dn", "d_dn", rawDown, lenDown);
            irLog = "Learned TEMP DOWN button: " + protocolName + " (" + String(count) + " pulses).";
            targetAssign = "none";
          }
        }
      }

      // --- NORMAL OPERATION (REMOTE DETECTION) ---
      else if (targetAssign == "none" && count >= MIN_AC_RAW_PULSES) {
        int misMode  = (lenMode > 0)  ? countMismatchedPulses(rawArray, count, rawMode, lenMode)   : 9999;
        int misPower = (lenPower > 0) ? countMismatchedPulses(rawArray, count, rawPower, lenPower) : 9999;
        int misUp    = (lenUp > 0)    ? countMismatchedPulses(rawArray, count, rawUp, lenUp)       : 9999;
        int misDown  = (lenDown > 0)  ? countMismatchedPulses(rawArray, count, rawDown, lenDown)   : 9999;

        // Find candidate with lowest pulse mismatches (tolerance: <= 4 mismatched pulses)
        int minMis = min(min(misPower, misMode), min(misUp, misDown));

        if (minMis <= 4) {
          if (minMis == misPower) {
            isSystemOn = !isSystemOn;
            irLog = "Remote detected: POWER TOGGLE (" + String(isSystemOn ? "ON" : "OFF") + ").";
          } else if (minMis == misUp) {
            targetTemp += 1.0f;
            if (targetTemp > 32.0f) targetTemp = 32.0f;
            prefs.begin("ac_ir", false);
            prefs.putFloat("tgt", targetTemp);
            prefs.end();
            isSystemOn = true;
            currentACMode = MODE_IS_COOL;
            lastSwitchTime = millis();
            irLog = "Remote detected: TEMP UP -> Target: " + String(targetTemp, 1) + " C.";
          } else if (minMis == misDown) {
            targetTemp -= 1.0f;
            if (targetTemp < 16.0f) targetTemp = 16.0f;
            prefs.begin("ac_ir", false);
            prefs.putFloat("tgt", targetTemp);
            prefs.end();
            isSystemOn = true;
            currentACMode = MODE_IS_COOL;
            lastSwitchTime = millis();
            irLog = "Remote detected: TEMP DOWN -> Target: " + String(targetTemp, 1) + " C.";
          } else if (minMis == misMode) {
            isSystemOn = true;
            irLog = "Remote detected: MODE button pressed.";
          }
        } else {
          // Unrecognized remote button or different state
          irLog = "Remote signal detected: " + protocolName + " (" + String(count) + " pulses, no match).";
        }
      }
    }

    delete[] rawArray;
    irrecv.resume();
  }

  // --- THERMOSTAT HYSTERESIS LOGIC ---
  if (isSystemOn && targetAssign == "none" && tempSensorReady && lenMode > 0) {
    unsigned long now = millis();
    float fanThreshold = getFanThreshold();     // e.g. 15.0 C (Target 16.0 - 1.0)
    float coolThreshold = getCoolThreshold();   // e.g. 18.0 C (FanThreshold 15.0 + Gap 3.0)

    // Cold condition (Temp <= fanThreshold) -> Press MODE 2 times to switch from Cool to Fan (Cool -> Dry -> Fan)
    if (currentACMode == MODE_IS_COOL && currentTemp <= fanThreshold && (now - lastSwitchTime >= MIN_COMPRESSOR_INTERVAL)) {
      switchToFan();
      currentACMode = MODE_IS_FAN;
      lastSwitchTime = now;
      irLog = "Target cold reached (" + String(currentTemp, 1) + " C <= " + String(fanThreshold, 1) + " C). Switched to FAN (Mode x2).";
    }
    // Warm condition (Temp > coolThreshold - 0.1f) -> Press MODE 1 time to switch from Fan back to Cool
    else if (currentACMode == MODE_IS_FAN && currentTemp > (coolThreshold - 0.1f) && (now - lastSwitchTime >= MIN_COMPRESSOR_INTERVAL)) {
      switchToCool();
      currentACMode = MODE_IS_COOL;
      lastSwitchTime = now;
      irLog = "Room warm reached (" + String(currentTemp, 1) + " C > " + String(coolThreshold - 0.1f, 1) + " C). Switched to COOL (Mode x1).";
    }
  }

  delay(2);
}