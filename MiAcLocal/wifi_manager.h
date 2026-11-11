#pragma once

#include <Arduino.h>
#include "settings_store.h"
#include "event_log.h"

enum class WifiModeState : uint8_t {
  AP_SETUP = 0,
  STA_CONNECTING,
  STA_CONNECTED,
  AP_STA_RECOVERY
};

// Manages STA static IP connection, SoftAP initial provisioning, fallback recovery, and BOOT reset.
class WifiManager {
public:
  void begin(const WifiSettings& cfg, SettingsStore& store, EventLog& log);
  void update();

  bool isStaConnected() const;
  bool isApActive() const;
  WifiModeState state() const { return _state; }
  String stateText() const;
  String activeIp() const;
  int rssi() const;

  // Scan nearby SSIDs and return a JSON array of objects [{"ssid":"...","rssi":-50,"enc":true}, ...].
  String scanNetworksJson();

private:
  void startSoftAp(bool withSta);
  void startSta();
  void checkBootButton();

  WifiSettings _cfg;
  SettingsStore* _store = nullptr;
  EventLog* _log = nullptr;

  WifiModeState _state = WifiModeState::AP_SETUP;
  uint32_t _connectStartMs = 0;
  uint32_t _lastRetryMs = 0;
  uint32_t _bootHoldStartMs = 0;
  bool _bootHoldActive = false;
};

