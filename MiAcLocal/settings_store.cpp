#include "settings_store.h"
#include <Preferences.h>
#include <IPAddress.h>
#include <math.h>
#include <ctype.h>

namespace {

const char* NS_APP = "miac";
const char* NS_WIFI = "wifi";
const char* NS_TELE = "tele";

void putIntIfChanged(Preferences& p, const char* key, int val, int defaultVal) {
  if (!p.isKey(key) || p.getInt(key, defaultVal) != val) {
    p.putInt(key, val);
  }
}

void putFloatIfChanged(Preferences& p, const char* key, float val, float defaultVal) {
  if (!p.isKey(key) || fabsf(p.getFloat(key, defaultVal) - val) > 0.01f) {
    p.putFloat(key, val);
  }
}

void putBoolIfChanged(Preferences& p, const char* key, bool val, bool defaultVal) {
  if (!p.isKey(key) || p.getBool(key, defaultVal) != val) {
    p.putBool(key, val);
  }
}

void putStringIfChanged(Preferences& p, const char* key, const String& val, const String& defaultVal) {
  if (!p.isKey(key) || p.getString(key, defaultVal) != val) {
    p.putString(key, val);
  }
}

}  // namespace

int SettingsStore::clampTarget(int targetC) {
  if (targetC < TARGET_MIN_C) return TARGET_MIN_C;
  if (targetC > TARGET_MAX_C) return TARGET_MAX_C;
  return targetC;
}

float SettingsStore::sanitizeGap(float gapC) {
  // Snap to 0.5 C steps before clamping
  float snapped = roundf(gapC * 2.0f) / 2.0f;
  if (snapped < GAP_MIN_C) snapped = GAP_MIN_C;
  if (snapped > GAP_MAX_C) snapped = GAP_MAX_C;
  return snapped;
}

bool SettingsStore::isValidIp(const String& ipStr) {
  IPAddress ip;
  return ip.fromString(ipStr);
}

bool SettingsStore::isValidToken(const String& tokenHex) {
  if (tokenHex.length() != 32) return false;
  for (size_t i = 0; i < tokenHex.length(); i++) {
    if (!isxdigit(static_cast<unsigned char>(tokenHex[i]))) return false;
  }
  return true;
}

void SettingsStore::loadApp(AppSettings& out) {
  Preferences p;
  if (!p.begin(NS_APP, false)) return;

  out.targetTemp = clampTarget(p.getInt("target", DEFAULT_TARGET_C));
  out.tempGap = sanitizeGap(p.getFloat("gap", DEFAULT_GAP_C));
  out.autoEnabled = p.getBool("autoEn", true);
  out.fanByEsp = p.getBool("fanByEsp", false);
  out.beepEnabled = p.getBool("beepEn", false);
  out.modeCool = p.getInt("modeCool", DEFAULT_MODE_COOL);
  out.modeDry = p.getInt("modeDry", DEFAULT_MODE_DRY);
  out.modeFan = p.getInt("modeFan", DEFAULT_MODE_FAN);

  String ip = p.getString("acIp", DEFAULT_AC_IP);
  out.acIp = isValidIp(ip) ? ip : String(DEFAULT_AC_IP);

  String tok = p.getString("token", DEFAULT_AC_TOKEN);
  tok.toLowerCase();
  out.acToken = isValidToken(tok) ? tok : String(DEFAULT_AC_TOKEN);

  p.end();
}

void SettingsStore::saveApp(const AppSettings& cfg) {
  Preferences p;
  if (!p.begin(NS_APP, false)) return;

  putIntIfChanged(p, "target", clampTarget(cfg.targetTemp), DEFAULT_TARGET_C);
  putFloatIfChanged(p, "gap", sanitizeGap(cfg.tempGap), DEFAULT_GAP_C);
  putBoolIfChanged(p, "autoEn", cfg.autoEnabled, true);
  putBoolIfChanged(p, "fanByEsp", cfg.fanByEsp, false);
  putBoolIfChanged(p, "beepEn", cfg.beepEnabled, false);
  putIntIfChanged(p, "modeCool", cfg.modeCool, DEFAULT_MODE_COOL);
  putIntIfChanged(p, "modeDry", cfg.modeDry, DEFAULT_MODE_DRY);
  putIntIfChanged(p, "modeFan", cfg.modeFan, DEFAULT_MODE_FAN);
  putStringIfChanged(p, "acIp", cfg.acIp, DEFAULT_AC_IP);
  putStringIfChanged(p, "token", cfg.acToken, DEFAULT_AC_TOKEN);

  p.end();
}

void SettingsStore::loadWifi(WifiSettings& out) {
  Preferences p;
  if (!p.begin(NS_WIFI, false)) return;

  out.ssid = p.getString("ssid", "");
  out.password = p.getString("pass", "");

  String ip = p.getString("ip", DEFAULT_STATIC_IP);
  out.staticIp = isValidIp(ip) ? ip : String(DEFAULT_STATIC_IP);

  String gw = p.getString("gw", DEFAULT_GATEWAY);
  out.gateway = isValidIp(gw) ? gw : String(DEFAULT_GATEWAY);

  String mask = p.getString("mask", DEFAULT_SUBNET);
  out.subnet = isValidIp(mask) ? mask : String(DEFAULT_SUBNET);

  String dns = p.getString("dns", DEFAULT_DNS);
  out.dns = isValidIp(dns) ? dns : String(DEFAULT_DNS);

  p.end();
}

void SettingsStore::saveWifi(const WifiSettings& cfg) {
  Preferences p;
  if (!p.begin(NS_WIFI, false)) return;

  putStringIfChanged(p, "ssid", cfg.ssid, "");
  putStringIfChanged(p, "pass", cfg.password, "");
  putStringIfChanged(p, "ip", cfg.staticIp, DEFAULT_STATIC_IP);
  putStringIfChanged(p, "gw", cfg.gateway, DEFAULT_GATEWAY);
  putStringIfChanged(p, "mask", cfg.subnet, DEFAULT_SUBNET);
  putStringIfChanged(p, "dns", cfg.dns, DEFAULT_DNS);

  p.end();
}

void SettingsStore::clearWifi() {
  Preferences p;
  if (!p.begin(NS_WIFI, false)) return;
  p.clear();
  p.end();
}

String SettingsStore::normalizeTursoUrl(const String& raw) {
  String s = raw;
  s.trim();
  while (s.endsWith("/")) {
    s.remove(s.length() - 1);
  }
  if (s.startsWith("libsql://")) {
    s = "https://" + s.substring(9);
  }
  return s;
}

bool SettingsStore::isValidDeviceId(const String& id) {
  if (id.length() == 0 || id.length() > 32) return false;
  for (size_t i = 0; i < id.length(); i++) {
    char c = id[i];
    if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

void SettingsStore::loadTelemetry(TelemetrySettings& out) {
  Preferences p;
  if (!p.begin(NS_TELE, true)) {
    out.enabled = false;
    out.url = normalizeTursoUrl(DEFAULT_TURSO_URL);
    out.token = DEFAULT_TURSO_TOKEN;
    out.deviceId = DEFAULT_DEVICE_ID;
    return;
  }

  out.enabled = p.getBool("enabled", false);
  out.url = normalizeTursoUrl(p.getString("url", DEFAULT_TURSO_URL));
  out.token = p.getString("token", DEFAULT_TURSO_TOKEN);
  String devId = p.getString("dev_id", DEFAULT_DEVICE_ID);
  out.deviceId = isValidDeviceId(devId) ? devId : String(DEFAULT_DEVICE_ID);

  p.end();
}

void SettingsStore::saveTelemetry(const TelemetrySettings& cfg) {
  Preferences p;
  if (!p.begin(NS_TELE, false)) return;

  putBoolIfChanged(p, "enabled", cfg.enabled, false);
  putStringIfChanged(p, "url", normalizeTursoUrl(cfg.url), "");
  putStringIfChanged(p, "token", cfg.token, "");
  putStringIfChanged(p, "dev_id", cfg.deviceId, DEFAULT_DEVICE_ID);

  p.end();
}


