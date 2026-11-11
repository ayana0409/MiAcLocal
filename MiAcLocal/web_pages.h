#pragma once

#include <pgmspace.h>

const char STYLE_CSS[] PROGMEM = R"rawliteral(
:root {
  color-scheme: dark;
  --bg: #0b1120;
  --surface: #131c31;
  --surface-alt: #0f172a;
  --border: #28354f;
  --text: #f1f5f9;
  --muted: #94a3b8;
  --primary: #3b82f6;
  --primary-hover: #2563eb;
  --cool: #06b6d4;
  --fan: #10b981;
  --dry: #a855f7;
  --warn: #f59e0b;
  --danger: #ef4444;
  accent-color: var(--primary);
}
* { box-sizing: border-box; }
body {
  margin: 0;
  padding: 16px;
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  background: var(--bg);
  color: var(--text);
  line-height: 1.5;
  display: flex;
  justify-content: center;
}
.wrap { width: 100%; max-width: 640px; }
.nav {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 16px;
  flex-wrap: wrap;
}
.brand { font-size: 1.2rem; font-weight: 700; margin: 0; }
.nav-links { display: flex; gap: 8px; }
.nav-links a {
  color: var(--muted);
  text-decoration: none;
  padding: 8px 12px;
  border-radius: 8px;
  border: 1px solid var(--border);
  font-size: 0.9rem;
  font-weight: 600;
  min-height: 40px;
  display: inline-flex;
  align-items: center;
}
.nav-links a.active, .nav-links a:hover {
  color: var(--text);
  border-color: var(--primary);
  background: rgba(59, 130, 246, 0.14);
}
.card {
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 16px;
  margin-bottom: 16px;
}
.card-title {
  margin: 0 0 12px 0;
  font-size: 1rem;
  font-weight: 700;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
}
.alert {
  border-radius: 10px;
  padding: 12px 14px;
  margin-bottom: 14px;
  font-size: 0.92rem;
  font-weight: 600;
  border: 1px solid;
}
.alert-danger { background: rgba(239, 68, 68, 0.14); border-color: var(--danger); color: #fecaca; }
.alert-warn { background: rgba(245, 158, 11, 0.14); border-color: var(--warn); color: #fde68a; }
.alert-ok { background: rgba(16, 185, 129, 0.14); border-color: var(--fan); color: #a7f3d0; }
.grid-2 {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 12px;
}
.grid-3 {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 12px;
}
.stat-box {
  background: var(--surface-alt);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 12px;
  text-align: center;
}
.stat-label { font-size: 0.8rem; color: var(--muted); }
.stat-val { font-size: 1.7rem; font-weight: 700; margin-top: 2px; }
.val-cool { color: var(--cool); }
.badge {
  display: inline-flex;
  align-items: center;
  padding: 4px 10px;
  border-radius: 999px;
  font-size: 0.78rem;
  font-weight: 700;
  letter-spacing: 0.02em;
}
.b-cool { background: rgba(6, 182, 212, 0.18); color: #67e8f9; }
.b-fan { background: rgba(16, 185, 129, 0.18); color: #6ee7b7; }
.b-warn { background: rgba(245, 158, 11, 0.18); color: #fcd34d; }
.b-danger { background: rgba(239, 68, 68, 0.18); color: #fca5a5; }
.b-muted { background: rgba(148, 163, 184, 0.18); color: #cbd5e1; }
.row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  padding: 10px 0;
  border-top: 1px solid rgba(40, 53, 79, 0.65);
  flex-wrap: wrap;
}
.row:first-of-type { border-top: none; padding-top: 0; }
.row:last-of-type { padding-bottom: 0; }
.row-label { font-weight: 600; font-size: 0.95rem; }
.row-sub { font-size: 0.82rem; color: var(--muted); }
.btn-group { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
button {
  font: inherit;
  font-size: 0.92rem;
  font-weight: 600;
  min-height: 44px;
  padding: 8px 14px;
  border-radius: 8px;
  border: 1px solid var(--border);
  background: var(--surface-alt);
  color: var(--text);
  cursor: pointer;
}
button:hover { border-color: var(--primary); }
button:focus-visible, input:focus-visible, select:focus-visible {
  outline: 2px solid var(--primary);
  outline-offset: 2px;
}
button.btn-primary {
  background: var(--primary);
  border-color: var(--primary);
  color: #fff;
}
button.btn-primary:hover { background: var(--primary-hover); }
button.btn-danger {
  background: rgba(239, 68, 68, 0.18);
  border-color: var(--danger);
  color: #fecaca;
}
button[aria-pressed="true"].toggle-cool { background: rgba(6, 182, 212, 0.25); border-color: var(--cool); color: #67e8f9; }
button[aria-pressed="true"].toggle-fan { background: rgba(16, 185, 129, 0.25); border-color: var(--fan); color: #6ee7b7; }
button[aria-pressed="true"].toggle-dry { background: rgba(168, 85, 247, 0.25); border-color: var(--dry); color: #e9d5ff; }
button[aria-pressed="true"].toggle-on { background: var(--primary); border-color: var(--primary); color: #fff; }
.step-val { min-width: 64px; text-align: center; font-size: 1.1rem; font-weight: 700; }
.field {
  display: flex;
  flex-direction: column;
  gap: 6px;
  margin-bottom: 14px;
}
.field label { font-size: 0.9rem; font-weight: 600; }
.hint { font-size: 0.8rem; color: var(--muted); }
input, select {
  font: inherit;
  font-size: max(1rem, 1em);
  min-height: 46px;
  padding: 8px 12px;
  border-radius: 8px;
  border: 1px solid var(--border);
  background: var(--surface-alt);
  color: var(--text);
  width: 100%;
}
.input-inline { display: flex; gap: 8px; align-items: stretch; }
.input-inline input, .input-inline select { flex: 1; }
:is(input, select):is(:user-invalid, .user-invalid-fallback) {
  border-color: var(--danger);
  background: rgba(239, 68, 68, 0.08);
}
.error-msg {
  display: none;
  color: #fca5a5;
  font-size: 0.82rem;
}
:is(input, select):is(:user-invalid, .user-invalid-fallback) ~ .error-msg {
  display: block;
}
.log-list {
  background: var(--surface-alt);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 10px 12px;
  max-height: 220px;
  overflow-y: auto;
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-size: 0.8rem;
}
.log-item { padding: 3px 0; border-bottom: 1px solid rgba(40, 53, 79, 0.4); }
.log-item:last-child { border-bottom: none; }
.log-t { color: var(--muted); margin-right: 8px; }
pre.json-box {
  background: var(--surface-alt);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 12px;
  overflow-x: auto;
  font-size: 0.82rem;
  margin: 10px 0 0 0;
  white-space: pre-wrap;
  word-break: break-word;
}
table { width: 100%; border-collapse: collapse; font-size: 0.86rem; margin-top: 10px; }
th, td { text-align: left; padding: 8px 6px; border-bottom: 1px solid var(--border); }
th { color: var(--muted); font-weight: 600; }
[hidden] { display: none !important; }
)rawliteral";

const char APP_JS[] PROGMEM = R"rawliteral(
function bindWifiScan(btnId, selectId, inputId) {
  const btn = document.getElementById(btnId);
  const sel = document.getElementById(selectId);
  const inp = document.getElementById(inputId);
  if (!btn || !sel || !inp) return;

  btn.addEventListener('click', async () => {
    btn.disabled = true;
    sel.innerHTML = '<option value="">Scanning...</option>';
    try {
      const r = await fetch('/api/wifi/scan');
      const nets = await r.json();
      sel.replaceChildren();
      const def = document.createElement('option');
      def.value = '';
      def.textContent = '-- Select a Wi-Fi network --';
      sel.appendChild(def);
      nets.forEach((n) => {
        const opt = document.createElement('option');
        opt.value = n.ssid;
        opt.textContent = n.ssid + ' (' + n.rssi + ' dBm)';
        sel.appendChild(opt);
      });
    } catch (e) {
      sel.innerHTML = '<option value="">Scan failed</option>';
    } finally {
      btn.disabled = false;
    }
  });

  sel.addEventListener('change', (e) => {
    if (e.target.value) inp.value = e.target.value;
  });
}

function initDeferredValidation(root) {
  const supportsUserInvalid = typeof CSS !== 'undefined' && CSS.supports('selector(:user-invalid)');
  const dirty = new WeakMap();
  const fields = root.querySelectorAll('input, select');

  const syncAria = (el, touched) => {
    const invalid = supportsUserInvalid ? el.matches(':user-invalid') : (touched && !el.validity.valid);
    if (!supportsUserInvalid && touched) {
      el.classList.toggle('user-invalid-fallback', invalid);
    }
    el.setAttribute('aria-invalid', invalid ? 'true' : 'false');
    const errId = el.dataset.error;
    if (errId) {
      const ids = (el.getAttribute('aria-describedby') || '').split(/\s+/).filter((x) => x && x !== errId);
      if (invalid) ids.push(errId);
      if (ids.length) el.setAttribute('aria-describedby', ids.join(' '));
      else el.removeAttribute('aria-describedby');
    }
  };

  fields.forEach((el) => {
    el.setAttribute('aria-invalid', 'false');
    el.addEventListener('input', () => {
      const st = dirty.get(el) || { i: false, b: false };
      st.i = true;
      dirty.set(el, st);
      syncAria(el, st.b);
    });
    el.addEventListener('blur', () => {
      const st = dirty.get(el) || { i: false, b: false };
      st.b = true;
      dirty.set(el, st);
      syncAria(el, st.i);
    });
    el.addEventListener('invalid', () => {
      dirty.set(el, { i: true, b: true });
      syncAria(el, true);
    });
  });
}
)rawliteral";

const char PAGE_INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <meta name="color-scheme" content="dark">
  <title>MiAC Local Controller</title>
  <link rel="stylesheet" href="/style.css">
</head>
<body>
  <div class="wrap">
    <nav class="nav" aria-label="Primary">
      <h1 class="brand">MiAC Local</h1>
      <div class="nav-links">
        <a href="/" class="active" aria-current="page">Dashboard</a>
        <a href="/settings">Settings</a>
        <a href="/debug">Debug</a>
      </div>
    </nav>

    <section id="ap-setup-card" class="card" hidden>
      <div class="card-title">
        <span>First-Time Wi-Fi Setup</span>
        <span class="badge b-warn">Setup AP</span>
      </div>
      <p class="row-sub" style="margin-top:0">ESP32 is running in <strong>MiAC-Setup</strong> mode. Connect it to the same Wi-Fi network as your air conditioner.</p>
      <div class="field">
        <label for="ap-scan-select">Nearby Wi-Fi Networks</label>
        <div class="input-inline">
          <select id="ap-scan-select" aria-label="Scanned Wi-Fi networks">
            <option value="">-- Click Scan to find networks --</option>
          </select>
          <button id="btn-ap-scan" type="button">Scan</button>
        </div>
      </div>
      <form id="ap-wifi-form" method="POST" action="/api/settings">
        <div class="field">
          <label for="ap-ssid">Wi-Fi SSID</label>
          <input id="ap-ssid" name="ssid" type="text" required maxlength="32" autocorrect="off" autocapitalize="none" spellcheck="false" data-error="ap-ssid-err">
          <span id="ap-ssid-err" class="error-msg">Error: Please enter a Wi-Fi network name.</span>
        </div>
        <div class="field">
          <label for="ap-pass">Wi-Fi Password</label>
          <input id="ap-pass" name="password" type="password" maxlength="64" autocomplete="current-password">
        </div>
        <div class="grid-2">
          <div class="field">
            <label for="ap-ip">ESP32 Static IP</label>
            <input id="ap-ip" name="staticIp" type="text" required value="192.168.1.50" pattern="^(?:(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\.){3}(?:25[0-5]|2[0-4]\d|[01]?\d\d?)$" autocorrect="off" autocapitalize="none" spellcheck="false" data-error="ap-ip-err">
            <span id="ap-ip-err" class="error-msg">Error: Enter a valid IPv4 address.</span>
          </div>
          <div class="field">
            <label for="ap-gw">Gateway</label>
            <input id="ap-gw" name="gateway" type="text" required value="192.168.1.1" pattern="^(?:(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\.){3}(?:25[0-5]|2[0-4]\d|[01]?\d\d?)$" autocorrect="off" autocapitalize="none" spellcheck="false" data-error="ap-gw-err">
            <span id="ap-gw-err" class="error-msg">Error: Enter a valid gateway IPv4.</span>
          </div>
        </div>
        <button type="submit" class="btn-primary" style="width:100%">Save Wi-Fi &amp; Reboot</button>
      </form>
      <div id="ap-msg" role="status" aria-live="polite" class="row-sub" style="margin-top:8px"></div>
    </section>

    <div id="alert-box" class="alert alert-danger" role="status" aria-live="polite" hidden></div>
    <div id="cmd-error" class="alert alert-warn" role="status" aria-live="polite" hidden></div>

    <section class="card" aria-label="Thermostat Status">
      <div class="card-title">
        <span>Thermostat</span>
        <span id="state-badge" class="badge b-muted">--</span>
      </div>

      <div class="grid-2" style="margin-bottom:12px">
        <div class="stat-box">
          <div class="stat-label">Room Temp (ESP32)</div>
          <div id="room-temp" class="stat-val val-cool">--.- &deg;C</div>
        </div>
        <div class="stat-box">
          <div class="stat-label">AC Internal Sensor</div>
          <div id="ac-temp" class="stat-val">-- &deg;C</div>
        </div>
      </div>

      <div class="row">
        <div>
          <div class="row-label">ESP32 Auto Control</div>
          <div id="state-reason" class="row-sub">Loading...</div>
        </div>
        <button id="btn-auto" type="button" class="toggle-on" aria-pressed="true">Auto: ON</button>
      </div>

      <div class="row">
        <div>
          <div class="row-label">Thresholds</div>
          <div id="thresh-info" class="row-sub">FAN &lt; 25.0 &deg;C | COOL &gt; 29.0 &deg;C</div>
        </div>
        <span id="lock-badge" class="badge b-muted">Protection: Ready</span>
      </div>
    </section>

    <section class="card" aria-label="AC Controls">
      <div class="card-title">
        <span>Air Conditioner</span>
        <span id="conn-badge" class="badge b-muted">Connecting...</span>
      </div>

      <div class="row">
        <span class="row-label">Power</span>
        <button id="btn-power" type="button" class="toggle-on" aria-pressed="false">OFF</button>
      </div>

      <div class="row">
        <span class="row-label">Mode</span>
        <div class="btn-group">
          <button id="btn-mode-cool" type="button" class="toggle-cool" aria-pressed="false">COOL</button>
          <button id="btn-mode-dry" type="button" class="toggle-dry" aria-pressed="false">DRY</button>
          <button id="btn-mode-fan" type="button" class="toggle-fan" aria-pressed="false">FAN</button>
        </div>
      </div>

      <div class="row">
        <div>
          <div class="row-label">Target Temp</div>
          <div class="row-sub">16&ndash;30 &deg;C (2-way sync)</div>
        </div>
        <div class="btn-group">
          <button id="btn-tgt-dn" type="button" aria-label="Decrease target temperature">&minus;</button>
          <span id="val-target" class="step-val">26 &deg;C</span>
          <button id="btn-tgt-up" type="button" aria-label="Increase target temperature">+</button>
        </div>
      </div>

      <div class="row">
        <div>
          <div class="row-label">Cooling Gap</div>
          <div class="row-sub">2.0&ndash;7.0 &deg;C (step 0.5)</div>
        </div>
        <div class="btn-group">
          <button id="btn-gap-dn" type="button" aria-label="Decrease gap">&minus;</button>
          <span id="val-gap" class="step-val">4.0 &deg;C</span>
          <button id="btn-gap-up" type="button" aria-label="Increase gap">+</button>
        </div>
      </div>

      <div class="row">
        <span class="row-label">Fan Speed</span>
        <div class="btn-group">
          <button id="btn-fan-low" type="button" class="toggle-on" aria-pressed="false">Low</button>
          <button id="btn-fan-high" type="button" class="toggle-on" aria-pressed="false">High</button>
        </div>
      </div>

      <div class="row">
        <span class="row-label">Display &amp; Sound</span>
        <div class="btn-group">
          <button id="btn-led" type="button" class="toggle-on" aria-pressed="false">LED: OFF</button>
          <button id="btn-beep" type="button" class="toggle-on" aria-pressed="false">Beep: OFF</button>
        </div>
      </div>
    </section>

    <section class="card" aria-label="System Info and Event Log">
      <div class="card-title">
        <span>Event Log</span>
        <span id="sys-meta" class="row-sub">--</span>
      </div>
      <div id="log-list" class="log-list"></div>
    </section>
  </div>

  <script src="/app.js"></script>
  <script>
    let cur = null;

    async function postForm(url, params) {
      const errEl = document.getElementById('cmd-error');
      errEl.hidden = true;
      const body = new URLSearchParams(params);
      try {
        const r = await fetch(url, {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body
        });
        if (!r.ok) {
          errEl.textContent = 'Command failed: ' + (await r.text());
          errEl.hidden = false;
        }
      } catch (e) {
        errEl.textContent = 'Network error sending command.';
        errEl.hidden = false;
      }
      await fetchState();
    }

    function setPressed(id, active, text) {
      const el = document.getElementById(id);
      el.setAttribute('aria-pressed', active ? 'true' : 'false');
      if (text !== undefined) el.textContent = text;
    }

    function renderState(d) {
      cur = d;
      document.getElementById('ap-setup-card').hidden = !(d.wifiApActive && !d.wifiConnected);

      const alertBox = document.getElementById('alert-box');
      const alerts = [];
      if (!d.acOnline) {
        alerts.push(d.tokenSuspected
          ? 'AC Offline: Token may be incorrect or rejected by AC.'
          : 'AC Offline: Waiting to reconnect to ' + (d.acIp || 'AC') + '.');
      }
      if (d.sensorFault) alerts.push('DS18B20 Fault: Safety FAN protection active.');
      if (d.modeVerifyFailed) alerts.push('Mode verification failed: Auto control was turned OFF.');
      if (alerts.length > 0) {
        alertBox.textContent = alerts.join(' ');
        alertBox.hidden = false;
      } else {
        alertBox.hidden = true;
      }

      const stBadge = document.getElementById('state-badge');
      stBadge.textContent = d.state;
      stBadge.className = 'badge ' + (
        d.state === 'AUTO_COOL' ? 'b-cool' :
        d.state === 'AUTO_FAN' ? 'b-fan' :
        d.state.startsWith('FAULT') || d.state === 'AC_OFFLINE' ? 'b-danger' :
        d.state === 'PAUSED_USER' ? 'b-warn' : 'b-muted'
      );

      document.getElementById('room-temp').textContent = d.sensorValid ? d.roomTemp.toFixed(1) + ' \u00B0C' : (d.sensorFault ? 'FAULT' : '--.- \u00B0C');
      document.getElementById('ac-temp').textContent = d.acOnline ? (d.acInternalTemp + ' \u00B0C') : '-- \u00B0C';
      document.getElementById('state-reason').textContent = d.reason;
      document.getElementById('thresh-info').textContent = 'FAN < ' + d.fanThresh.toFixed(1) + ' \u00B0C | COOL > ' + d.coolThresh.toFixed(1) + ' \u00B0C';

      const lockBadge = document.getElementById('lock-badge');
      if (d.lockRemSec > 0) {
        lockBadge.textContent = (d.lockType === 'MIN_RUN' ? 'Min Run: ' : 'Min Rest: ') + d.lockRemSec + 's';
        lockBadge.className = 'badge b-warn';
      } else {
        lockBadge.textContent = 'Protection: Ready';
        lockBadge.className = 'badge b-muted';
      }

      setPressed('btn-auto', d.autoEnabled, d.autoEnabled ? 'Auto: ON' : 'Auto: OFF');

      const connBadge = document.getElementById('conn-badge');
      connBadge.textContent = d.acOnline ? 'AC Online' : 'AC Offline';
      connBadge.className = 'badge ' + (d.acOnline ? 'b-fan' : 'b-danger');

      setPressed('btn-power', d.acPower, d.acPower ? 'ON' : 'OFF');
      setPressed('btn-mode-cool', d.acPower && d.acMode === d.modeCool);
      setPressed('btn-mode-dry', d.acPower && d.acMode === d.modeDry);
      setPressed('btn-mode-fan', d.acPower && d.acMode === d.modeFan);

      document.getElementById('val-target').textContent = d.target + ' \u00B0C';
      document.getElementById('val-gap').textContent = d.gap.toFixed(1) + ' \u00B0C';

      setPressed('btn-fan-low', d.acFan === 0);
      setPressed('btn-fan-high', d.acFan === 1);
      setPressed('btn-led', d.acLed, 'LED: ' + (d.acLed ? 'ON' : 'OFF'));
      setPressed('btn-beep', d.beepSetting, 'Beep: ' + (d.beepSetting ? 'ON' : 'OFF'));

      document.getElementById('sys-meta').textContent = d.wifiIp + ' | Up ' + d.uptime;

      const list = document.getElementById('log-list');
      list.replaceChildren();
      (d.logs || []).forEach((item) => {
        const div = document.createElement('div');
        div.className = 'log-item';
        const tSpan = document.createElement('span');
        tSpan.className = 'log-t';
        tSpan.textContent = item.t;
        div.appendChild(tSpan);
        div.appendChild(document.createTextNode(item.m));
        list.appendChild(div);
      });
    }

    async function fetchState() {
      try {
        const r = await fetch('/api/state');
        if (r.ok) renderState(await r.json());
      } catch (e) {}
    }

    document.getElementById('btn-auto').addEventListener('click', () => {
      if (cur) postForm('/api/auto', { enabled: cur.autoEnabled ? '0' : '1' });
    });
    document.getElementById('btn-power').addEventListener('click', () => {
      if (cur) postForm('/api/control', { power: cur.acPower ? '0' : '1' });
    });
    document.getElementById('btn-mode-cool').addEventListener('click', () => postForm('/api/control', { mode: 'cool' }));
    document.getElementById('btn-mode-dry').addEventListener('click', () => postForm('/api/control', { mode: 'dry' }));
    document.getElementById('btn-mode-fan').addEventListener('click', () => postForm('/api/control', { mode: 'fan' }));
    document.getElementById('btn-tgt-dn').addEventListener('click', () => {
      if (cur && cur.target > 16) postForm('/api/control', { target: String(cur.target - 1) });
    });
    document.getElementById('btn-tgt-up').addEventListener('click', () => {
      if (cur && cur.target < 30) postForm('/api/control', { target: String(cur.target + 1) });
    });
    document.getElementById('btn-gap-dn').addEventListener('click', () => {
      if (cur && cur.gap > 2.0) postForm('/api/config', { gap: (cur.gap - 0.5).toFixed(1) });
    });
    document.getElementById('btn-gap-up').addEventListener('click', () => {
      if (cur && cur.gap < 7.0) postForm('/api/config', { gap: (cur.gap + 0.5).toFixed(1) });
    });
    document.getElementById('btn-fan-low').addEventListener('click', () => postForm('/api/control', { fan: '0' }));
    document.getElementById('btn-fan-high').addEventListener('click', () => postForm('/api/control', { fan: '1' }));
    document.getElementById('btn-led').addEventListener('click', () => {
      if (cur) postForm('/api/control', { led: cur.acLed ? '0' : '1' });
    });
    document.getElementById('btn-beep').addEventListener('click', () => {
      if (cur) postForm('/api/control', { beep: cur.beepSetting ? '0' : '1' });
    });

    bindWifiScan('btn-ap-scan', 'ap-scan-select', 'ap-ssid');
    initDeferredValidation(document.getElementById('ap-wifi-form'));
    document.getElementById('ap-wifi-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const data = new URLSearchParams(new FormData(e.target));
      const r = await fetch('/api/settings', { method: 'POST', body: data });
      document.getElementById('ap-msg').textContent = await r.text();
    });

    fetchState();
    setInterval(fetchState, 2000);
  </script>
</body>
</html>
)rawliteral";

const char PAGE_SETTINGS_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <meta name="color-scheme" content="dark">
  <title>Settings - MiAC Local</title>
  <link rel="stylesheet" href="/style.css">
</head>
<body>
  <div class="wrap">
    <nav class="nav" aria-label="Primary">
      <h1 class="brand">MiAC Local</h1>
      <div class="nav-links">
        <a href="/">Dashboard</a>
        <a href="/settings" class="active" aria-current="page">Settings</a>
        <a href="/debug">Debug</a>
      </div>
    </nav>

    <div id="save-status" class="alert alert-ok" role="status" aria-live="polite" hidden></div>

    <section class="card">
      <h2 class="card-title">AC &amp; MIoT Configuration</h2>
      <form id="ac-form" method="POST" action="/api/settings">
        <div class="field">
          <label for="acIp">AC IP Address</label>
          <span id="acIp-hint" class="hint">IPv4 address reserved for the AC on your router.</span>
          <input id="acIp" name="acIp" type="text" required pattern="^(?:(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\.){3}(?:25[0-5]|2[0-4]\d|[01]?\d\d?)$" autocorrect="off" autocapitalize="none" spellcheck="false" aria-describedby="acIp-hint" data-error="acIp-err">
          <span id="acIp-err" class="error-msg">Error: Enter a valid IPv4 address (e.g. 192.168.1.100).</span>
        </div>

        <div class="field">
          <label for="token">New miIO Token (Optional)</label>
          <span id="token-hint" class="hint">32 hex characters. Leave empty to keep the stored token (hidden for security).</span>
          <input id="token" name="token" type="text" pattern="^[0-9a-fA-F]{32}$" minlength="32" maxlength="32" autocorrect="off" autocapitalize="none" spellcheck="false" aria-describedby="token-hint" placeholder="Leave blank to keep existing" data-error="token-err">
          <span id="token-err" class="error-msg">Error: Token must be exactly 32 hex characters (0-9, a-f).</span>
        </div>

        <div class="grid-3">
          <div class="field">
            <label for="modeCool">COOL Code</label>
            <input id="modeCool" name="modeCool" type="number" min="0" max="20" step="1" required data-error="mc-err">
            <span id="mc-err" class="error-msg">Error: Integer 0&ndash;20.</span>
          </div>
          <div class="field">
            <label for="modeDry">DRY Code</label>
            <input id="modeDry" name="modeDry" type="number" min="0" max="20" step="1" required data-error="md-err">
            <span id="md-err" class="error-msg">Error: Integer 0&ndash;20.</span>
          </div>
          <div class="field">
            <label for="modeFan">FAN Code</label>
            <input id="modeFan" name="modeFan" type="number" min="0" max="20" step="1" required data-error="mf-err">
            <span id="mf-err" class="error-msg">Error: Integer 0&ndash;20.</span>
          </div>
        </div>

        <button type="submit" class="btn-primary">Save AC Settings</button>
      </form>
    </section>

    <section class="card">
      <h2 class="card-title">Wi-Fi &amp; Static IP</h2>
      <div class="field">
        <label for="scan-select">Scan Nearby Networks</label>
        <div class="input-inline">
          <select id="scan-select" aria-label="Scanned Wi-Fi networks">
            <option value="">-- Click Scan --</option>
          </select>
          <button id="btn-scan" type="button">Scan</button>
        </div>
      </div>

      <form id="wifi-form" method="POST" action="/api/settings">
        <div class="field">
          <label for="ssid">Wi-Fi SSID</label>
          <input id="ssid" name="ssid" type="text" required maxlength="32" autocorrect="off" autocapitalize="none" spellcheck="false" data-error="ssid-err">
          <span id="ssid-err" class="error-msg">Error: Please enter your Wi-Fi SSID.</span>
        </div>

        <div class="field">
          <label for="password">Wi-Fi Password</label>
          <div class="input-inline">
            <input id="password" name="password" type="password" maxlength="64" autocomplete="current-password">
            <button id="btn-toggle-pw" type="button" aria-pressed="false" aria-label="Show password">Show</button>
          </div>
        </div>

        <div class="grid-2">
          <div class="field">
            <label for="staticIp">Static IP</label>
            <input id="staticIp" name="staticIp" type="text" required pattern="^(?:(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\.){3}(?:25[0-5]|2[0-4]\d|[01]?\d\d?)$" autocorrect="off" autocapitalize="none" spellcheck="false" data-error="sip-err">
            <span id="sip-err" class="error-msg">Error: Valid IPv4 required.</span>
          </div>
          <div class="field">
            <label for="gateway">Gateway</label>
            <input id="gateway" name="gateway" type="text" required pattern="^(?:(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\.){3}(?:25[0-5]|2[0-4]\d|[01]?\d\d?)$" autocorrect="off" autocapitalize="none" spellcheck="false" data-error="gw-err">
            <span id="gw-err" class="error-msg">Error: Valid IPv4 required.</span>
          </div>
          <div class="field">
            <label for="subnet">Subnet Mask</label>
            <input id="subnet" name="subnet" type="text" required pattern="^(?:(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\.){3}(?:25[0-5]|2[0-4]\d|[01]?\d\d?)$" autocorrect="off" autocapitalize="none" spellcheck="false" data-error="sub-err">
            <span id="sub-err" class="error-msg">Error: Valid IPv4 required.</span>
          </div>
          <div class="field">
            <label for="dns">DNS Server</label>
            <input id="dns" name="dns" type="text" required pattern="^(?:(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\.){3}(?:25[0-5]|2[0-4]\d|[01]?\d\d?)$" autocorrect="off" autocapitalize="none" spellcheck="false" data-error="dns-err">
            <span id="dns-err" class="error-msg">Error: Valid IPv4 required.</span>
          </div>
        </div>

        <div class="btn-group">
          <button type="submit" class="btn-primary">Save Wi-Fi &amp; Reboot</button>
          <button id="btn-reboot" type="button" class="btn-danger">Reboot ESP32</button>
        </div>
      </form>
    </section>
  </div>

  <script src="/app.js"></script>
  <script>
    function showBanner(msg, ok) {
      const box = document.getElementById('save-status');
      box.textContent = msg;
      box.className = 'alert ' + (ok ? 'alert-ok' : 'alert-danger');
      box.hidden = false;
    }

    async function loadSettings() {
      const r = await fetch('/api/settings');
      if (!r.ok) return;
      const s = await r.json();
      document.getElementById('acIp').value = s.acIp || '';
      document.getElementById('modeCool').value = s.modeCool;
      document.getElementById('modeDry').value = s.modeDry;
      document.getElementById('modeFan').value = s.modeFan;
      document.getElementById('ssid').value = s.ssid || '';
      document.getElementById('staticIp').value = s.staticIp || '';
      document.getElementById('gateway').value = s.gateway || '';
      document.getElementById('subnet').value = s.subnet || '';
      document.getElementById('dns').value = s.dns || '';
    }

    initDeferredValidation(document.getElementById('ac-form'));
    initDeferredValidation(document.getElementById('wifi-form'));
    bindWifiScan('btn-scan', 'scan-select', 'ssid');

    document.getElementById('ac-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const body = new URLSearchParams(new FormData(e.target));
      const r = await fetch('/api/settings', { method: 'POST', body });
      showBanner(await r.text(), r.ok);
      if (r.ok) document.getElementById('token').value = '';
    });

    document.getElementById('wifi-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const body = new URLSearchParams(new FormData(e.target));
      const r = await fetch('/api/settings', { method: 'POST', body });
      showBanner(await r.text(), r.ok);
    });

    document.getElementById('btn-toggle-pw').addEventListener('click', (e) => {
      const pw = document.getElementById('password');
      const show = pw.type === 'password';
      pw.type = show ? 'text' : 'password';
      e.currentTarget.setAttribute('aria-pressed', show ? 'true' : 'false');
      e.currentTarget.textContent = show ? 'Hide' : 'Show';
    });

    document.getElementById('btn-reboot').addEventListener('click', async () => {
      if (!confirm('Reboot ESP32 now?')) return;
      const r = await fetch('/api/reboot', { method: 'POST' });
      showBanner(await r.text(), r.ok);
    });

    loadSettings();
  </script>
</body>
</html>
)rawliteral";

const char PAGE_DEBUG_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <meta name="color-scheme" content="dark">
  <title>MIoT Debug - MiAC Local</title>
  <link rel="stylesheet" href="/style.css">
</head>
<body>
  <div class="wrap">
    <nav class="nav" aria-label="Primary">
      <h1 class="brand">MiAC Local</h1>
      <div class="nav-links">
        <a href="/">Dashboard</a>
        <a href="/settings">Settings</a>
        <a href="/debug" class="active" aria-current="page">Debug</a>
      </div>
    </nav>

    <section class="card">
      <h2 class="card-title">miIO.info</h2>
      <button id="btn-info" type="button" class="btn-primary">Query miIO.info</button>
      <pre id="out-info" class="json-box">Click to query device info...</pre>
    </section>

    <section class="card">
      <h2 class="card-title">Raw MIoT Properties</h2>
      <button id="btn-props" type="button" class="btn-primary">Read All 7 Properties</button>
      <table>
        <thead><tr><th>Property</th><th>siid</th><th>piid</th><th>code</th><th>value</th></tr></thead>
        <tbody id="props-body"></tbody>
      </table>
      <pre id="out-props" class="json-box"></pre>
    </section>

    <section class="card">
      <h2 class="card-title">Raw set_properties</h2>
      <form id="raw-set-form" method="POST" action="/api/debug/set">
        <div class="grid-3">
          <div class="field">
            <label for="siid">siid</label>
            <input id="siid" name="siid" type="number" required min="1" max="99" value="2">
          </div>
          <div class="field">
            <label for="piid">piid</label>
            <input id="piid" name="piid" type="number" required min="1" max="99" value="2">
          </div>
          <div class="field">
            <label for="ptype">Type</label>
            <select id="ptype" name="type">
              <option value="int">int</option>
              <option value="bool">bool</option>
              <option value="float">float</option>
            </select>
          </div>
        </div>
        <div class="field">
          <label for="pvalue">Value</label>
          <input id="pvalue" name="value" type="text" required placeholder="e.g. 0 or true">
        </div>
        <button type="submit" class="btn-danger">Send Raw Set</button>
      </form>
      <pre id="out-set" class="json-box">Ready.</pre>
    </section>
  </div>

  <script>
    const PROP_NAMES = { '2.1':'power', '2.2':'mode', '2.4':'target_temp', '3.2':'fan_level', '4.1':'room_temp', '5.1':'beep', '6.1':'led' };

    document.getElementById('btn-info').addEventListener('click', async () => {
      const out = document.getElementById('out-info');
      out.textContent = 'Querying...';
      const r = await fetch('/api/debug/info');
      const txt = await r.text();
      try { out.textContent = JSON.stringify(JSON.parse(txt), null, 2); }
      catch { out.textContent = txt; }
    });

    document.getElementById('btn-props').addEventListener('click', async () => {
      const out = document.getElementById('out-props');
      const tbody = document.getElementById('props-body');
      out.textContent = 'Reading...';
      const r = await fetch('/api/debug/props');
      const txt = await r.text();
      out.textContent = txt;
      tbody.replaceChildren();
      try {
        const arr = JSON.parse(txt);
        if (Array.isArray(arr)) {
          arr.forEach((p) => {
            const tr = document.createElement('tr');
            const name = PROP_NAMES[p.siid + '.' + p.piid] || p.did || '';
            [name, p.siid, p.piid, p.code, p.value].forEach((v) => {
              const td = document.createElement('td');
              td.textContent = String(v);
              tr.appendChild(td);
            });
            tbody.appendChild(tr);
          });
        }
      } catch (e) {}
    });

    document.getElementById('raw-set-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const out = document.getElementById('out-set');
      out.textContent = 'Sending...';
      const body = new URLSearchParams(new FormData(e.target));
      const r = await fetch('/api/debug/set', { method: 'POST', body });
      out.textContent = await r.text();
    });
  </script>
</body>
</html>
)rawliteral";
