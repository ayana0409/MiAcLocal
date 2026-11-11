#include "wifi_manager.h"
#include "config.h"
#include "json_lite.h"
#include <WiFi.h>

void WifiManager::begin(const WifiSettings& cfg, SettingsStore& store, EventLog& log) {
  _cfg = cfg;
  _store = &store;
  _log = &log;

  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  if (_cfg.ssid.length() == 0) {
    startSoftAp(false);
    _state = WifiModeState::AP_SETUP;
    _log->add("Wi-Fi not configured: started setup AP '" + String(AP_SSID) + "' at " + String(AP_IP_ADDR));
  } else {
    startSta();
  }
}

void WifiManager::startSoftAp(bool withSta) {
  IPAddress apIp;
  IPAddress subnet(255, 255, 255, 0);
  apIp.fromString(AP_IP_ADDR);

  WiFi.mode(withSta ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAPConfig(apIp, apIp, subnet);
  WiFi.softAP(AP_SSID, AP_PASS);
}

void WifiManager::startSta() {
  IPAddress ip, gw, mask, dns;
  ip.fromString(_cfg.staticIp);
  gw.fromString(_cfg.gateway);
  mask.fromString(_cfg.subnet);
  dns.fromString(_cfg.dns);

  WiFi.mode(WIFI_STA);
  if (!WiFi.config(ip, gw, mask, dns)) {
    _log->add("WARNING: WiFi.config static IP failed");
  }
  WiFi.begin(_cfg.ssid.c_str(), _cfg.password.c_str());

  _state = WifiModeState::STA_CONNECTING;
  _connectStartMs = millis();
  _lastRetryMs = millis();
  _log->add("Connecting Wi-Fi '" + _cfg.ssid + "' (static IP " + _cfg.staticIp + ")...");
}

void WifiManager::update() {
  checkBootButton();

  if (_state == WifiModeState::AP_SETUP) {
    return;
  }

  const uint32_t now = millis();
  const bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected) {
    if (_state != WifiModeState::STA_CONNECTED) {
      if (_state == WifiModeState::AP_STA_RECOVERY) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
      }
      _state = WifiModeState::STA_CONNECTED;
      _log->add("Wi-Fi connected: " + WiFi.localIP().toString() + " (RSSI " + String(WiFi.RSSI()) + " dBm)");
    }
    return;
  }

  // Handle link drop from connected state
  if (_state == WifiModeState::STA_CONNECTED) {
    _state = WifiModeState::STA_CONNECTING;
    _connectStartMs = now;
    _lastRetryMs = now;
    _log->add("Wi-Fi disconnected, attempting reconnect...");
    return;
  }

  if (_state == WifiModeState::STA_CONNECTING) {
    if (now - _connectStartMs >= WIFI_CONNECT_TIMEOUT_MS) {
      startSoftAp(true);
      _state = WifiModeState::AP_STA_RECOVERY;
      _lastRetryMs = now;
      _log->add("Wi-Fi timeout (60s): started recovery AP '" + String(AP_SSID) + "' alongside STA retry");
    }
    return;
  }

  if (_state == WifiModeState::AP_STA_RECOVERY) {
    // Only kick STA reconnect when no user is actively connected to the recovery AP
    if (WiFi.softAPgetStationNum() == 0 && (now - _lastRetryMs >= WIFI_RETRY_INTERVAL_MS)) {
      _lastRetryMs = now;
      WiFi.begin(_cfg.ssid.c_str(), _cfg.password.c_str());
    }
  }
}

void WifiManager::checkBootButton() {
  if (digitalRead(PIN_BOOT_BUTTON) == LOW) {
    if (!_bootHoldActive) {
      _bootHoldActive = true;
      _bootHoldStartMs = millis();
    } else if (millis() - _bootHoldStartMs >= BOOT_RESET_HOLD_MS) {
      _log->add("BOOT button held 5s: clearing Wi-Fi settings and rebooting...");
      _store->clearWifi();
      delay(300);
      ESP.restart();
    }
  } else {
    _bootHoldActive = false;
  }
}

bool WifiManager::isStaConnected() const {
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

bool WifiManager::isApActive() const {
  return _state == WifiModeState::AP_SETUP || _state == WifiModeState::AP_STA_RECOVERY;
}

String WifiManager::stateText() const {
  switch (_state) {
    case WifiModeState::AP_SETUP:        return "AP Setup";
    case WifiModeState::STA_CONNECTING:  return "Connecting";
    case WifiModeState::STA_CONNECTED:   return "Connected";
    case WifiModeState::AP_STA_RECOVERY: return "AP + STA Recovery";
  }
  return "Unknown";
}

String WifiManager::activeIp() const {
  if (isStaConnected()) return WiFi.localIP().toString();
  if (isApActive()) return WiFi.softAPIP().toString();
  return _cfg.staticIp;
}

int WifiManager::rssi() const {
  return isStaConnected() ? WiFi.RSSI() : 0;
}

String WifiManager::scanNetworksJson() {
  // Ensure STA radio is enabled for scanning even while in pure AP_SETUP
  if (_state == WifiModeState::AP_SETUP) {
    WiFi.mode(WIFI_AP_STA);
  }
  int n = WiFi.scanNetworks(false, false);
  String json = "[";
  bool first = true;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    // Deduplicate identical SSIDs from mesh/repeaters (scanNetworks sorts by RSSI desc)
    bool duplicate = false;
    for (int j = 0; j < i; j++) {
      if (WiFi.SSID(j) == ssid) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;

    if (!first) json += ",";
    first = false;
    json += "{\"ssid\":\"";
    json += JsonLite::escapeString(ssid);
    json += "\",\"rssi\":";
    json += String(WiFi.RSSI(i));
    json += ",\"enc\":";
    json += (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? "true" : "false";
    json += "}";
  }
  json += "]";
  WiFi.scanDelete();
  return json;
}

