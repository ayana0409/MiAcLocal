#pragma once

#include <Arduino.h>
#include "config.h"
#include "settings_store.h"
#include "miio_client.h"
#include "event_log.h"

struct AcState {
  bool online = false;
  bool everPolled = false;

  // Filtered values (respect 5s pending-write suppression so Web UI stays stable)
  bool power = false;
  int mode = DEFAULT_MODE_COOL;
  int targetTemp = DEFAULT_TARGET_C;
  int fanLevel = FAN_LEVEL_LOW;
  int roomTemp = 0;
  bool beep = false;
  bool led = true;

  // Raw values directly from the most recent successful AC poll
  bool rawPower = false;
  int rawMode = DEFAULT_MODE_COOL;
  int rawTargetTemp = DEFAULT_TARGET_C;
  int rawFanLevel = FAN_LEVEL_LOW;
  bool rawBeep = false;

  uint32_t lastSuccessMs = 0;
  int consecutiveFails = 0;
  bool tokenSuspected = false;
  String lastError;
};

// High-level MIoT interface for Xiaomi New Widetech Pro (nwt.airc.wap31a).
class AcDevice {
public:
  bool begin(const AppSettings& cfg, EventLog& log);
  bool reconfigure(const AppSettings& cfg);

  // Drives async polling; returns true when a fresh poll result was processed.
  bool update(bool wifiConnected, EventLog& log);
  void requestImmediatePoll();

  const AcState& state() const { return _state; }
  bool isModePendingWrite() const;
  bool isTargetPendingWrite() const;

  // Synchronous property writes with 5s optimistic readback suppression
  bool setPower(bool on);
  bool setMode(int rawMode);
  bool setModeAndTarget(int rawMode, int targetC);
  bool setTargetTemp(int targetC);
  bool setFanLevel(int level);
  bool setBeep(bool on);
  bool setLed(bool on);

  // Debug helpers for /debug page
  bool fetchInfoJson(String& outJson, String& outError);
  bool fetchRawPropsJson(String& outJson, String& outError);
  bool setRawProperty(int siid, int piid, const String& valueJson, String& outResp, String& outError);

  uint32_t deviceId() const { return _miio.deviceId(); }
  uint32_t stamp() const { return _miio.currentStamp(); }

private:
  template <typename T>
  struct PendingProp {
    bool active = false;
    T expected = T();
    uint32_t untilMs = 0;

    void arm(T val, uint32_t until) {
      active = true;
      expected = val;
      untilMs = until;
    }

    bool isActive(uint32_t now) const {
      return active && (static_cast<int32_t>(untilMs - now) > 0);
    }

    void apply(T rawVal, T& filteredOut, uint32_t now) {
      if (active) {
        if (rawVal == expected || static_cast<int32_t>(untilMs - now) <= 0) {
          active = false;
          filteredOut = rawVal;
        }
      } else {
        filteredOut = rawVal;
      }
    }
  };

  void startPoll();
  bool parseGetPropsResponse(const String& resultJson);
  bool sendSetProperties(const String& paramsJson);
  bool writeIntProp(const char* did, int siid, int piid, int val, int& filteredOut, PendingProp<int>& pending);
  bool writeBoolProp(const char* did, int siid, int piid, bool val, bool& filteredOut, PendingProp<bool>& pending);
  static bool parseBoolToken(const String& token, bool& outVal);

  MiioClient _miio;
  AcState _state;
  EventLog* _log = nullptr;

  bool _pollInFlight = false;
  uint32_t _lastPollStartMs = 0;

  PendingProp<bool> _pendPower;
  PendingProp<int> _pendMode;
  PendingProp<int> _pendTarget;
  PendingProp<int> _pendFan;
  PendingProp<bool> _pendBeep;
  PendingProp<bool> _pendLed;
};
