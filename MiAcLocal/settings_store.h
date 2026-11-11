#pragma once

#include <Arduino.h>
#include "config.h"

struct AppSettings {
  int targetTemp = DEFAULT_TARGET_C;
  float tempGap = DEFAULT_GAP_C;
  bool autoEnabled = true;
  bool fanByEsp = false;
  bool beepEnabled = false;
  int modeCool = DEFAULT_MODE_COOL;
  int modeDry = DEFAULT_MODE_DRY;
  int modeFan = DEFAULT_MODE_FAN;
  String acIp = DEFAULT_AC_IP;
  String acToken = DEFAULT_AC_TOKEN;
};

struct WifiSettings {
  String ssid = "";
  String password = "";
  String staticIp = DEFAULT_STATIC_IP;
  String gateway = DEFAULT_GATEWAY;
  String subnet = DEFAULT_SUBNET;
  String dns = DEFAULT_DNS;
};

struct TelemetrySettings {
  bool enabled = false;
  String url = DEFAULT_TURSO_URL;
  String token = DEFAULT_TURSO_TOKEN;
  String deviceId = DEFAULT_DEVICE_ID;
};

// Persistent NVS storage wrapper (writes only when values change).
class SettingsStore {
public:
  void loadApp(AppSettings& out);
  void saveApp(const AppSettings& cfg);

  void loadWifi(WifiSettings& out);
  void saveWifi(const WifiSettings& cfg);
  void clearWifi();

  void loadTelemetry(TelemetrySettings& out);
  void saveTelemetry(const TelemetrySettings& cfg);

  static int clampTarget(int targetC);
  static float sanitizeGap(float gapC);
  static bool isValidIp(const String& ipStr);
  static bool isValidToken(const String& tokenHex);
  static String normalizeTursoUrl(const String& raw);
  static bool isValidDeviceId(const String& id);
};

