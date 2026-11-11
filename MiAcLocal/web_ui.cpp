#include "web_ui.h"
#include "web_pages.h"
#include "json_lite.h"
#include <stdlib.h>
#include <math.h>

WebUi::WebUi()
  : _server(WEB_SERVER_PORT) {}

void WebUi::begin(AppSettings& appCfg,
                  WifiSettings& wifiCfg,
                  SettingsStore& store,
                  WifiManager& wifi,
                  TempSensor& sensor,
                  AcDevice& ac,
                  Thermostat& thermostat,
                  EventLog& log) {
  _appCfg = &appCfg;
  _wifiCfg = &wifiCfg;
  _store = &store;
  _wifi = &wifi;
  _sensor = &sensor;
  _ac = &ac;
  _thermostat = &thermostat;
  _log = &log;

  registerRoutes();
  _server.begin();
  _log->add("HTTP server started on port " + String(WEB_SERVER_PORT));
}

void WebUi::update() {
  _server.handleClient();
  if (_rebootPending && static_cast<int32_t>(millis() - _rebootAtMs) >= 0) {
    ESP.restart();
  }
}

void WebUi::registerRoutes() {
  _server.on("/", HTTP_GET, [this]() {
    _server.send_P(200, "text/html", PAGE_INDEX_HTML);
  });
  _server.on("/settings", HTTP_GET, [this]() {
    _server.send_P(200, "text/html", PAGE_SETTINGS_HTML);
  });
  _server.on("/debug", HTTP_GET, [this]() {
    _server.send_P(200, "text/html", PAGE_DEBUG_HTML);
  });
  _server.on("/style.css", HTTP_GET, [this]() {
    _server.sendHeader("Cache-Control", "max-age=3600");
    _server.send_P(200, "text/css", STYLE_CSS);
  });
  _server.on("/app.js", HTTP_GET, [this]() {
    _server.sendHeader("Cache-Control", "max-age=3600");
    _server.send_P(200, "application/javascript", APP_JS);
  });

  _server.on("/api/state", HTTP_GET, [this]() { handleState(); });
  _server.on("/api/control", HTTP_POST, [this]() { handleControl(); });
  _server.on("/api/auto", HTTP_POST, [this]() { handleAuto(); });
  _server.on("/api/config", HTTP_POST, [this]() { handleConfig(); });
  _server.on("/api/settings", HTTP_GET, [this]() { handleGetSettings(); });
  _server.on("/api/settings", HTTP_POST, [this]() { handlePostSettings(); });
  _server.on("/api/wifi/scan", HTTP_GET, [this]() { handleWifiScan(); });
  _server.on("/api/debug/info", HTTP_GET, [this]() { handleDebugInfo(); });
  _server.on("/api/debug/props", HTTP_GET, [this]() { handleDebugProps(); });
  _server.on("/api/debug/set", HTTP_POST, [this]() { handleDebugSet(); });
  _server.on("/api/reboot", HTTP_POST, [this]() { handleReboot(); });

  _server.onNotFound([this]() {
    _server.send(404, "text/plain", "Not Found");
  });
}

bool WebUi::parseStrictInt(const String& s, int& out) {
  String t = s;
  t.trim();
  if (t.length() == 0) return false;
  char* end = nullptr;
  long val = strtol(t.c_str(), &end, 10);
  if (end == t.c_str() || *end != '\0') return false;
  out = static_cast<int>(val);
  return true;
}

bool WebUi::parseStrictFloat(const String& s, float& out) {
  String t = s;
  t.trim();
  if (t.length() == 0) return false;
  char* end = nullptr;
  float val = strtof(t.c_str(), &end);
  if (end == t.c_str() || *end != '\0' || !isfinite(val)) return false;
  out = val;
  return true;
}

bool WebUi::parseStrictBool(const String& s, bool& out) {
  String t = s;
  t.trim();
  t.toLowerCase();
  if (t == "1" || t == "true" || t == "on") {
    out = true;
    return true;
  }
  if (t == "0" || t == "false" || t == "off") {
    out = false;
    return true;
  }
  return false;
}

void WebUi::handleState() {
  const AcState& ac = _ac->state();
  String j;
  j.reserve(1200);

  j += "{";
  j += "\"wifiConnected\":"; j += (_wifi->isStaConnected() ? "true" : "false");
  j += ",\"wifiApActive\":"; j += (_wifi->isApActive() ? "true" : "false");
  j += ",\"wifiState\":\""; j += JsonLite::escapeString(_wifi->stateText()); j += "\"";
  j += ",\"wifiIp\":\""; j += JsonLite::escapeString(_wifi->activeIp()); j += "\"";
  j += ",\"wifiRssi\":"; j += String(_wifi->rssi());
  j += ",\"uptime\":\""; j += JsonLite::formatUptime(millis()); j += "\"";

  j += ",\"sensorValid\":"; j += (_sensor->hasValidTemp() ? "true" : "false");
  j += ",\"sensorFault\":"; j += (_sensor->isFault() ? "true" : "false");
  j += ",\"roomTemp\":"; j += String(_sensor->hasValidTemp() ? _sensor->tempC() : 0.0f, 1);

  j += ",\"acIp\":\""; j += JsonLite::escapeString(_appCfg->acIp); j += "\"";
  j += ",\"acOnline\":"; j += (ac.online ? "true" : "false");
  j += ",\"tokenSuspected\":"; j += (ac.tokenSuspected ? "true" : "false");
  j += ",\"acPower\":"; j += (ac.power ? "true" : "false");
  j += ",\"acMode\":"; j += String(ac.mode);
  j += ",\"acTarget\":"; j += String(ac.targetTemp);
  j += ",\"acFan\":"; j += String(ac.fanLevel);
  j += ",\"acInternalTemp\":"; j += String(ac.roomTemp);
  j += ",\"acBeep\":"; j += (ac.beep ? "true" : "false");
  j += ",\"acLed\":"; j += (ac.led ? "true" : "false");

  j += ",\"autoEnabled\":"; j += (_appCfg->autoEnabled ? "true" : "false");
  j += ",\"target\":"; j += String(_appCfg->targetTemp);
  j += ",\"gap\":"; j += String(_appCfg->tempGap, 1);
  j += ",\"fanThresh\":"; j += String(_thermostat->fanThreshold(), 1);
  j += ",\"coolThresh\":"; j += String(_thermostat->coolThreshold(), 1);
  j += ",\"beepSetting\":"; j += (_appCfg->beepEnabled ? "true" : "false");
  j += ",\"modeCool\":"; j += String(_appCfg->modeCool);
  j += ",\"modeDry\":"; j += String(_appCfg->modeDry);
  j += ",\"modeFan\":"; j += String(_appCfg->modeFan);

  j += ",\"state\":\""; j += _thermostat->stateName(); j += "\"";
  j += ",\"reason\":\""; j += JsonLite::escapeString(_thermostat->reason()); j += "\"";
  j += ",\"lockType\":\""; j += _thermostat->lockType(); j += "\"";
  j += ",\"lockRemSec\":"; j += String(_thermostat->lockRemainingSec());
  j += ",\"modeVerifyFailed\":"; j += (_thermostat->modeVerifyFailed() ? "true" : "false");
  j += ",\"acError\":\""; j += JsonLite::escapeString(ac.lastError); j += "\"";
  j += ",\"logs\":"; j += _log->toJsonArray();
  j += "}";

  _server.send(200, "application/json", j);
}

void WebUi::handleControl() {
  bool anyHandled = false;

  if (_server.hasArg("power")) {
    anyHandled = true;
    bool on = false;
    if (!parseStrictBool(_server.arg("power"), on)) {
      _server.send(400, "text/plain", "Invalid power value");
      return;
    }
    if (!_ac->state().online) {
      _server.send(503, "text/plain", "AC is offline");
      return;
    }
    if (!_ac->setPower(on)) {
      _server.send(502, "text/plain", _ac->state().lastError);
      return;
    }
    _thermostat->notifyWebPowerChanged(on);
  }

  if (_server.hasArg("mode")) {
    anyHandled = true;
    String m = _server.arg("mode");
    m.trim();
    m.toLowerCase();
    int rawMode = -1;
    if (m == "cool") rawMode = _appCfg->modeCool;
    else if (m == "dry") rawMode = _appCfg->modeDry;
    else if (m == "fan") rawMode = _appCfg->modeFan;
    else {
      _server.send(400, "text/plain", "Invalid mode (expected cool/dry/fan)");
      return;
    }
    if (!_ac->state().online) {
      _server.send(503, "text/plain", "AC is offline");
      return;
    }
    if (!_ac->setMode(rawMode)) {
      _server.send(502, "text/plain", _ac->state().lastError);
      return;
    }
    _thermostat->notifyWebModeChanged(rawMode);
  }

  if (_server.hasArg("target")) {
    anyHandled = true;
    int tgt = 0;
    if (!parseStrictInt(_server.arg("target"), tgt) || tgt < TARGET_MIN_C || tgt > TARGET_MAX_C) {
      _server.send(400, "text/plain", "Target must be integer 16..30");
      return;
    }
    if (!_thermostat->setTargetFromWeb(tgt)) {
      _server.send(502, "text/plain", _ac->state().lastError);
      return;
    }
  }

  if (_server.hasArg("fan")) {
    anyHandled = true;
    int fan = -1;
    if (!parseStrictInt(_server.arg("fan"), fan) || (fan != FAN_LEVEL_LOW && fan != FAN_LEVEL_HIGH)) {
      _server.send(400, "text/plain", "Fan level must be 0 (Low) or 1 (High)");
      return;
    }
    if (!_ac->state().online) {
      _server.send(503, "text/plain", "AC is offline");
      return;
    }
    if (!_ac->setFanLevel(fan)) {
      _server.send(502, "text/plain", _ac->state().lastError);
      return;
    }
    _log->add("Fan speed set via Web -> " + String(fan == FAN_LEVEL_LOW ? "Low (0)" : "High (1)"));
  }

  if (_server.hasArg("led")) {
    anyHandled = true;
    bool ledOn = false;
    if (!parseStrictBool(_server.arg("led"), ledOn)) {
      _server.send(400, "text/plain", "Invalid led value");
      return;
    }
    if (!_ac->state().online) {
      _server.send(503, "text/plain", "AC is offline");
      return;
    }
    if (!_ac->setLed(ledOn)) {
      _server.send(502, "text/plain", _ac->state().lastError);
      return;
    }
    _log->add(String("LED set via Web -> ") + (ledOn ? "ON" : "OFF"));
  }

  if (_server.hasArg("beep")) {
    anyHandled = true;
    bool beepOn = false;
    if (!parseStrictBool(_server.arg("beep"), beepOn)) {
      _server.send(400, "text/plain", "Invalid beep value");
      return;
    }
    if (!_thermostat->setBeepFromWeb(beepOn)) {
      _server.send(502, "text/plain", _ac->state().lastError);
      return;
    }
  }

  if (!anyHandled) {
    _server.send(400, "text/plain", "No control parameter provided");
    return;
  }
  _server.send(200, "text/plain", "OK");
}

void WebUi::handleAuto() {
  if (!_server.hasArg("enabled")) {
    _server.send(400, "text/plain", "Missing enabled parameter");
    return;
  }
  bool en = false;
  if (!parseStrictBool(_server.arg("enabled"), en)) {
    _server.send(400, "text/plain", "Invalid enabled value");
    return;
  }
  _thermostat->setAutoEnabled(en);
  _server.send(200, "text/plain", "OK");
}

void WebUi::handleConfig() {
  if (!_server.hasArg("gap")) {
    _server.send(400, "text/plain", "Missing gap parameter");
    return;
  }
  float gap = 0.0f;
  if (!parseStrictFloat(_server.arg("gap"), gap) || gap < GAP_MIN_C || gap > GAP_MAX_C) {
    _server.send(400, "text/plain", "Gap must be 2.0..7.0");
    return;
  }
  _thermostat->setGapFromWeb(gap);
  _server.send(200, "text/plain", "OK");
}

void WebUi::handleGetSettings() {
  // Intentionally omit acToken so the stored token is never exposed over HTTP
  String j = "{";
  j += "\"acIp\":\""; j += JsonLite::escapeString(_appCfg->acIp); j += "\"";
  j += ",\"modeCool\":"; j += String(_appCfg->modeCool);
  j += ",\"modeDry\":"; j += String(_appCfg->modeDry);
  j += ",\"modeFan\":"; j += String(_appCfg->modeFan);
  j += ",\"ssid\":\""; j += JsonLite::escapeString(_wifiCfg->ssid); j += "\"";
  j += ",\"staticIp\":\""; j += JsonLite::escapeString(_wifiCfg->staticIp); j += "\"";
  j += ",\"gateway\":\""; j += JsonLite::escapeString(_wifiCfg->gateway); j += "\"";
  j += ",\"subnet\":\""; j += JsonLite::escapeString(_wifiCfg->subnet); j += "\"";
  j += ",\"dns\":\""; j += JsonLite::escapeString(_wifiCfg->dns); j += "\"";
  j += "}";
  _server.send(200, "application/json", j);
}

void WebUi::handlePostSettings() {
  bool updatedAc = false;
  bool updatedWifi = false;

  // Validate & apply AC configuration if any AC fields are present
  if (_server.hasArg("acIp") || _server.hasArg("token") ||
      _server.hasArg("modeCool") || _server.hasArg("modeDry") || _server.hasArg("modeFan")) {
    AppSettings next = *_appCfg;

    if (_server.hasArg("acIp")) {
      String ip = _server.arg("acIp");
      ip.trim();
      if (!SettingsStore::isValidIp(ip)) {
        _server.send(400, "text/plain", "Invalid AC IPv4 address");
        return;
      }
      next.acIp = ip;
    }

    if (_server.hasArg("token")) {
      String tok = _server.arg("token");
      tok.trim();
      tok.toLowerCase();
      if (tok.length() > 0) {
        if (!SettingsStore::isValidToken(tok)) {
          _server.send(400, "text/plain", "Token must be 32 hex characters");
          return;
        }
        next.acToken = tok;
      }
    }

    if (_server.hasArg("modeCool")) {
      if (!parseStrictInt(_server.arg("modeCool"), next.modeCool) || next.modeCool < 0 || next.modeCool > 20) {
        _server.send(400, "text/plain", "Invalid COOL mode code");
        return;
      }
    }
    if (_server.hasArg("modeDry")) {
      if (!parseStrictInt(_server.arg("modeDry"), next.modeDry) || next.modeDry < 0 || next.modeDry > 20) {
        _server.send(400, "text/plain", "Invalid DRY mode code");
        return;
      }
    }
    if (_server.hasArg("modeFan")) {
      if (!parseStrictInt(_server.arg("modeFan"), next.modeFan) || next.modeFan < 0 || next.modeFan > 20) {
        _server.send(400, "text/plain", "Invalid FAN mode code");
        return;
      }
    }

    if (next.modeCool == next.modeDry || next.modeCool == next.modeFan || next.modeDry == next.modeFan) {
      _server.send(400, "text/plain", "COOL, DRY, and FAN mode codes must be distinct");
      return;
    }

    bool connChanged = (next.acIp != _appCfg->acIp) || (next.acToken != _appCfg->acToken);
    *_appCfg = next;
    _store->saveApp(*_appCfg);
    if (connChanged) {
      _ac->reconfigure(*_appCfg);
      _ac->requestImmediatePoll();
    }
    _log->add("AC settings updated");
    updatedAc = true;
  }

  // Validate & apply Wi-Fi configuration if SSID is present
  if (_server.hasArg("ssid")) {
    WifiSettings nextWifi = *_wifiCfg;
    String ssid = _server.arg("ssid");
    ssid.trim();
    if (ssid.length() == 0 || ssid.length() > 32) {
      _server.send(400, "text/plain", "SSID must be 1..32 characters");
      return;
    }
    nextWifi.ssid = ssid;

    if (_server.hasArg("password")) {
      String pw = _server.arg("password");
      if (pw.length() > 64) {
        _server.send(400, "text/plain", "Password too long");
        return;
      }
      nextWifi.password = pw;
    }

    auto validateIpArg = [this](const char* name, String& target) -> bool {
      if (!_server.hasArg(name)) return true;
      String val = _server.arg(name);
      val.trim();
      if (!SettingsStore::isValidIp(val)) return false;
      target = val;
      return true;
    };

    if (!validateIpArg("staticIp", nextWifi.staticIp) ||
        !validateIpArg("gateway", nextWifi.gateway) ||
        !validateIpArg("subnet", nextWifi.subnet) ||
        !validateIpArg("dns", nextWifi.dns)) {
      _server.send(400, "text/plain", "Invalid static IP/Gateway/Subnet/DNS format");
      return;
    }

    *_wifiCfg = nextWifi;
    _store->saveWifi(*_wifiCfg);
    _log->add("Wi-Fi saved (" + _wifiCfg->ssid + "), rebooting...");
    _rebootPending = true;
    _rebootAtMs = millis() + 800UL;
    updatedWifi = true;
  }

  if (!updatedAc && !updatedWifi) {
    _server.send(400, "text/plain", "No settings provided");
    return;
  }

  if (updatedWifi) {
    _server.send(200, "text/plain", "Wi-Fi settings saved. Rebooting to http://" + _wifiCfg->staticIp + " ...");
  } else {
    _server.send(200, "text/plain", "AC settings saved.");
  }
}

void WebUi::handleWifiScan() {
  _server.send(200, "application/json", _wifi->scanNetworksJson());
}

void WebUi::handleDebugInfo() {
  if (!_wifi->isStaConnected()) {
    _server.send(503, "text/plain", "Wi-Fi STA not connected");
    return;
  }
  String json, err;
  if (_ac->fetchInfoJson(json, err)) {
    _server.send(200, "application/json", json);
  } else {
    _server.send(502, "text/plain", "miIO.info failed: " + err);
  }
}

void WebUi::handleDebugProps() {
  if (!_wifi->isStaConnected()) {
    _server.send(503, "text/plain", "Wi-Fi STA not connected");
    return;
  }
  String json, err;
  if (_ac->fetchRawPropsJson(json, err)) {
    _server.send(200, "application/json", json);
  } else {
    _server.send(502, "text/plain", "get_properties failed: " + err);
  }
}

void WebUi::handleDebugSet() {
  if (!_wifi->isStaConnected()) {
    _server.send(503, "text/plain", "Wi-Fi STA not connected");
    return;
  }
  if (!_server.hasArg("siid") || !_server.hasArg("piid") ||
      !_server.hasArg("value") || !_server.hasArg("type")) {
    _server.send(400, "text/plain", "Missing siid, piid, type, or value");
    return;
  }

  int siid = 0, piid = 0;
  if (!parseStrictInt(_server.arg("siid"), siid) || siid < 1 || siid > 99 ||
      !parseStrictInt(_server.arg("piid"), piid) || piid < 1 || piid > 99) {
    _server.send(400, "text/plain", "Invalid siid or piid");
    return;
  }

  String type = _server.arg("type");
  String rawVal = _server.arg("value");
  String jsonVal;
  if (type == "bool") {
    bool b = false;
    if (!parseStrictBool(rawVal, b)) {
      _server.send(400, "text/plain", "Invalid bool value");
      return;
    }
    jsonVal = b ? "true" : "false";
  } else if (type == "int") {
    int iv = 0;
    if (!parseStrictInt(rawVal, iv)) {
      _server.send(400, "text/plain", "Invalid int value");
      return;
    }
    jsonVal = String(iv);
  } else if (type == "float") {
    float fv = 0.0f;
    if (!parseStrictFloat(rawVal, fv)) {
      _server.send(400, "text/plain", "Invalid float value");
      return;
    }
    jsonVal = String(fv, 2);
  } else {
    _server.send(400, "text/plain", "Type must be bool, int, or float");
    return;
  }

  String resp, err;
  if (_ac->setRawProperty(siid, piid, jsonVal, resp, err)) {
    _ac->requestImmediatePoll();
    _server.send(200, "application/json", resp);
  } else {
    _server.send(502, "text/plain", "set_properties failed: " + err);
  }
}

void WebUi::handleReboot() {
  _log->add("Reboot requested via Web UI");
  _rebootPending = true;
  _rebootAtMs = millis() + 500UL;
  _server.send(200, "text/plain", "Rebooting ESP32...");
}

