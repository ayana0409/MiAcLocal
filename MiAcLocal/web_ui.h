#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include "config.h"
#include "settings_store.h"
#include "wifi_manager.h"
#include "temp_sensor.h"
#include "ac_device.h"
#include "thermostat.h"
#include "event_log.h"

// Embedded HTTP server exposing Dashboard, Settings, Debug pages and JSON REST APIs.
class WebUi {
public:
  WebUi();
  void begin(AppSettings& appCfg,
             WifiSettings& wifiCfg,
             SettingsStore& store,
             WifiManager& wifi,
             TempSensor& sensor,
             AcDevice& ac,
             Thermostat& thermostat,
             EventLog& log);
  void update();

private:
  void registerRoutes();
  void handleState();
  void handleControl();
  void handleAuto();
  void handleConfig();
  void handleGetSettings();
  void handlePostSettings();
  void handleWifiScan();
  void handleDebugInfo();
  void handleDebugProps();
  void handleDebugSet();
  void handleReboot();

  static bool parseStrictInt(const String& s, int& out);
  static bool parseStrictFloat(const String& s, float& out);
  static bool parseStrictBool(const String& s, bool& out);

  WebServer _server;
  AppSettings* _appCfg = nullptr;
  WifiSettings* _wifiCfg = nullptr;
  SettingsStore* _store = nullptr;
  WifiManager* _wifi = nullptr;
  TempSensor* _sensor = nullptr;
  AcDevice* _ac = nullptr;
  Thermostat* _thermostat = nullptr;
  EventLog* _log = nullptr;

  bool _rebootPending = false;
  uint32_t _rebootAtMs = 0;
};

