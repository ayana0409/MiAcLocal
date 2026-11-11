#pragma once

#include <Arduino.h>
#include "config.h"
#include "settings_store.h"
#include "ac_device.h"
#include "temp_sensor.h"
#include "event_log.h"

enum class ControlState : uint8_t {
  AC_OFFLINE = 0,
  AC_OFF,
  AUTO_DISABLED,
  PAUSED_USER,
  AUTO_COOL,
  AUTO_FAN,
  FAULT_COOL_WAIT,
  FAULT_FAN,
  FAULT_MANUAL
};

// Thermostat state machine with compressor protection, remote/web 2-way sync,
// 10s temperature noise filtering, 5s mode debounce, and DS18B20 fault handling.
class Thermostat {
public:
  void begin(AppSettings& cfg, SettingsStore& store, AcDevice& ac, TempSensor& sensor, EventLog& log);
  void update(bool pollUpdated);

  // Explicit user actions from Web UI
  void setAutoEnabled(bool enabled);
  bool setTargetFromWeb(int targetC);
  void setGapFromWeb(float gapC);
  bool setBeepFromWeb(bool enabled);
  void notifyWebPowerChanged(bool on);
  void notifyWebModeChanged(int rawMode);

  ControlState state() const { return _state; }
  String stateName() const;
  const String& reason() const { return _reason; }
  bool modeVerifyFailed() const { return _modeVerifyFailed; }

  float fanThreshold() const;
  float coolThreshold() const;
  uint32_t lockRemainingSec() const;
  String lockType() const;

private:
  void evaluateInitialOrReconnect(uint32_t now);
  void updateCompressorTracking(uint32_t now);
  void handlePowerTransition(bool newPower, uint32_t now);
  void handleUserModeChange(int newMode, uint32_t now);
  void syncTargetFromRemote(uint32_t now);
  void syncBeepSetting(uint32_t now);
  void handleEspModeVerification(uint32_t now);
  void handleSensorFaultTransitions(uint32_t now);
  void runThermostatHysteresis(uint32_t now);

  bool commandEspMode(int targetMode, bool toFan, ControlState nextState, const String& logMsg, uint32_t now);
  bool isKnownMode(int rawMode) const;
  void setFanByEsp(bool val);
  void resetTempTimers();
  uint32_t getRunLockRemainingMs(uint32_t now) const;
  uint32_t getRestLockRemainingMs(uint32_t now) const;

  AppSettings* _cfg = nullptr;
  SettingsStore* _store = nullptr;
  AcDevice* _ac = nullptr;
  TempSensor* _sensor = nullptr;
  EventLog* _log = nullptr;

  ControlState _state = ControlState::AC_OFFLINE;
  String _reason = "Waiting for AC connection";

  bool _initializedFromAc = false;
  bool _lastRawPower = false;
  bool _lastRawRunning = false;

  uint32_t _coolStartMs = 0;
  uint32_t _restStartMs = 0;

  // Mode debounce (5s) for remote button cycling
  int _confirmedMode = DEFAULT_MODE_COOL;
  int _candidateMode = DEFAULT_MODE_COOL;
  uint32_t _candidateSinceMs = 0;

  // Temperature breach confirmation (10s)
  uint32_t _coldSinceMs = 0;
  uint32_t _warmSinceMs = 0;

  // ESP32-initiated mode change verification (10s) & fan-level preservation
  bool _espVerifyActive = false;
  int _espExpectedMode = DEFAULT_MODE_COOL;
  int _savedFanLevel = FAN_LEVEL_LOW;
  uint32_t _espVerifyStartedMs = 0;
  uint32_t _espVerifyDeadlineMs = 0;
  bool _modeVerifyFailed = false;

  // Deferred target sync when changed on Web UI while AC is in FAN or OFF
  bool _pendingTargetSync = false;
  uint32_t _lastBeepSyncMs = 0;
  int _warnedUnknownMode = -999;
};

