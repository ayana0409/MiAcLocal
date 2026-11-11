#include "thermostat.h"

void Thermostat::begin(AppSettings& cfg, SettingsStore& store, AcDevice& ac, TempSensor& sensor, EventLog& log) {
  _cfg = &cfg;
  _store = &store;
  _ac = &ac;
  _sensor = &sensor;
  _log = &log;

  _state = _cfg->autoEnabled ? ControlState::AC_OFFLINE : ControlState::AUTO_DISABLED;
  _reason = _cfg->autoEnabled ? "Waiting for AC connection" : "Auto control disabled";
}

float Thermostat::fanThreshold() const {
  return static_cast<float>(_cfg->targetTemp) - FAN_OFFSET_C;
}

float Thermostat::coolThreshold() const {
  return fanThreshold() + _cfg->tempGap;
}

bool Thermostat::isKnownMode(int rawMode) const {
  return rawMode == _cfg->modeCool || rawMode == _cfg->modeDry || rawMode == _cfg->modeFan;
}

void Thermostat::setFanByEsp(bool val) {
  if (_cfg->fanByEsp != val) {
    _cfg->fanByEsp = val;
    _store->saveApp(*_cfg);
  }
}

void Thermostat::resetTempTimers() {
  _coldSinceMs = 0;
  _warmSinceMs = 0;
}

uint32_t Thermostat::getRunLockRemainingMs(uint32_t now) const {
  if (_coolStartMs == 0) return 0;
  uint32_t elapsed = now - _coolStartMs;
  return (elapsed < MIN_COMPRESSOR_RUN_MS) ? (MIN_COMPRESSOR_RUN_MS - elapsed) : 0;
}

uint32_t Thermostat::getRestLockRemainingMs(uint32_t now) const {
  if (_restStartMs == 0) return 0;
  uint32_t elapsed = now - _restStartMs;
  return (elapsed < MIN_COMPRESSOR_REST_MS) ? (MIN_COMPRESSOR_REST_MS - elapsed) : 0;
}

uint32_t Thermostat::lockRemainingSec() const {
  const uint32_t now = millis();
  uint32_t ms = 0;
  if (_state == ControlState::AUTO_COOL || _state == ControlState::FAULT_COOL_WAIT) {
    ms = getRunLockRemainingMs(now);
  } else if (_state == ControlState::AUTO_FAN) {
    ms = getRestLockRemainingMs(now);
  }
  return (ms + 999UL) / 1000UL;
}

String Thermostat::lockType() const {
  if ((_state == ControlState::AUTO_COOL || _state == ControlState::FAULT_COOL_WAIT) &&
      getRunLockRemainingMs(millis()) > 0) {
    return "MIN_RUN";
  }
  if (_state == ControlState::AUTO_FAN && getRestLockRemainingMs(millis()) > 0) {
    return "MIN_REST";
  }
  return "NONE";
}

String Thermostat::stateName() const {
  switch (_state) {
    case ControlState::AC_OFFLINE:      return "AC_OFFLINE";
    case ControlState::AC_OFF:          return "AC_OFF";
    case ControlState::AUTO_DISABLED:   return "AUTO_DISABLED";
    case ControlState::PAUSED_USER:     return "PAUSED_USER";
    case ControlState::AUTO_COOL:       return "AUTO_COOL";
    case ControlState::AUTO_FAN:        return "AUTO_FAN";
    case ControlState::FAULT_COOL_WAIT: return "FAULT_COOL_WAIT";
    case ControlState::FAULT_FAN:       return "FAULT_FAN";
    case ControlState::FAULT_MANUAL:    return "FAULT_MANUAL";
  }
  return "UNKNOWN";
}

void Thermostat::updateCompressorTracking(uint32_t now) {
  const AcState& st = _ac->state();
  const bool rawRunning = st.online && st.rawPower && (st.rawMode == _cfg->modeCool);

  if (rawRunning && !_lastRawRunning) {
    _coolStartMs = now;
  } else if (!rawRunning && _lastRawRunning) {
    _restStartMs = now;
  }
  _lastRawRunning = rawRunning;
}

void Thermostat::evaluateInitialOrReconnect(uint32_t now) {
  const AcState& st = _ac->state();
  _initializedFromAc = true;
  _lastRawPower = st.rawPower;
  _lastRawRunning = st.rawPower && (st.rawMode == _cfg->modeCool);
  _confirmedMode = st.rawMode;
  _candidateMode = st.rawMode;
  _candidateSinceMs = 0;
  _espVerifyActive = false;
  resetTempTimers();

  if (!st.rawPower) {
    _coolStartMs = 0;
    _restStartMs = 0;
    setFanByEsp(false);
    _state = ControlState::AC_OFF;
    _reason = "AC is turned off";
    return;
  }

  if (_lastRawRunning) {
    // AC already running in COOL at boot/reconnect: lock 3 minutes before allowing switch to FAN
    _coolStartMs = now;
    _restStartMs = 0;
  } else if (st.rawMode == _cfg->modeFan && _cfg->fanByEsp) {
    // Resuming ESP32-owned FAN after reboot: rest duration is unknown, lock 3 minutes
    _restStartMs = now;
    _coolStartMs = 0;
  }

  if (!_cfg->autoEnabled) {
    _state = ControlState::AUTO_DISABLED;
    _reason = "Auto control disabled";
    return;
  }

  if (st.rawMode == _cfg->modeCool) {
    if (_sensor->isFault()) {
      _state = ControlState::FAULT_COOL_WAIT;
      _reason = "Sensor fault: holding COOL for 3-min protection";
    } else {
      _state = ControlState::AUTO_COOL;
      _reason = "Monitoring room temperature in COOL";
    }
    _log->add("Synced on AC COOL (3-min protection active)");
  } else if (st.rawMode == _cfg->modeFan) {
    if (_cfg->fanByEsp) {
      if (_sensor->isFault()) {
        _state = ControlState::FAULT_FAN;
        _reason = "Sensor fault: holding FAN";
      } else {
        _state = ControlState::AUTO_FAN;
        _reason = "Monitoring room temperature in FAN (resumed)";
      }
      _log->add("Resumed ESP32-managed FAN mode");
    } else {
      _state = ControlState::PAUSED_USER;
      _reason = "Paused: AC started in user FAN mode";
      _log->add("AC in user FAN mode: auto paused");
    }
  } else {
    setFanByEsp(false);
    _state = ControlState::PAUSED_USER;
    _reason = isKnownMode(st.rawMode) ? "Paused: AC in DRY mode" : "Paused: unknown AC mode";
  }
}

void Thermostat::handlePowerTransition(bool newPower, uint32_t now) {
  _lastRawPower = newPower;
  _espVerifyActive = false;
  _candidateSinceMs = 0;
  resetTempTimers();

  if (!newPower) {
    setFanByEsp(false);
    _state = ControlState::AC_OFF;
    _reason = "AC turned off";
    _log->add("AC powered OFF");
    return;
  }

  // AC just powered ON
  const AcState& st = _ac->state();
  _confirmedMode = st.mode;
  _candidateMode = st.mode;

  if (!_cfg->autoEnabled) {
    setFanByEsp(false);
    _state = ControlState::AUTO_DISABLED;
    _reason = "Auto control disabled";
    _log->add("AC powered ON (auto disabled)");
    return;
  }

  if (st.mode == _cfg->modeCool) {
    _coolStartMs = now;
    setFanByEsp(false);
    if (_pendingTargetSync) {
      if (_ac->setTargetTemp(_cfg->targetTemp)) {
        _pendingTargetSync = false;
      }
    }
    if (_sensor->isFault()) {
      _state = ControlState::FAULT_COOL_WAIT;
      _reason = "Sensor fault: holding COOL for 3-min protection";
    } else {
      _state = ControlState::AUTO_COOL;
      _reason = "AC powered ON in COOL (3-min protection active)";
    }
    _log->add("AC powered ON in COOL (locked 3 min)");
  } else if (st.mode == _cfg->modeFan) {
    // Rule: powering ON in FAN is always treated as user-selected FAN
    setFanByEsp(false);
    _state = ControlState::PAUSED_USER;
    _reason = "Paused: AC powered ON in FAN";
    _log->add("AC powered ON in FAN -> paused (user FAN)");
  } else {
    setFanByEsp(false);
    _state = ControlState::PAUSED_USER;
    _reason = isKnownMode(st.mode) ? "Paused: AC in DRY mode" : "Paused: unknown AC mode";
    _log->add("AC powered ON in mode " + String(st.mode) + " -> paused");
  }
}

void Thermostat::handleUserModeChange(int newMode, uint32_t now) {
  resetTempTimers();

  if (!isKnownMode(newMode)) {
    setFanByEsp(false);
    _state = ControlState::PAUSED_USER;
    _reason = "Paused: unknown AC mode (" + String(newMode) + ")";
    if (_warnedUnknownMode != newMode) {
      _warnedUnknownMode = newMode;
      _log->add("Warning: unknown AC mode code " + String(newMode));
    }
    return;
  }
  _warnedUnknownMode = -999;

  if (!_cfg->autoEnabled) {
    setFanByEsp(false);
    _state = ControlState::AUTO_DISABLED;
    _reason = "Auto control disabled";
    return;
  }

  if (newMode == _cfg->modeCool) {
    // Always enforce 3-minute minimum run whenever user brings AC back to COOL
    _coolStartMs = now;
    setFanByEsp(false);

    if (_pendingTargetSync) {
      if (_ac->setTargetTemp(_cfg->targetTemp)) {
        _pendingTargetSync = false;
      }
    }

    if (_sensor->isFault() || _state == ControlState::FAULT_FAN || _state == ControlState::FAULT_MANUAL) {
      _state = ControlState::FAULT_MANUAL;
      _reason = "Manual COOL during/after sensor fault (no auto intervention until mode change)";
      _log->add("User selected COOL during/after fault -> manual COOL");
    } else {
      _state = ControlState::AUTO_COOL;
      _reason = "User selected COOL (3-min protection active)";
      _log->add("User selected COOL (locked 3 min)");
    }
  } else if (newMode == _cfg->modeFan) {
    setFanByEsp(false);
    _state = ControlState::PAUSED_USER;
    _reason = "Paused: user selected FAN";
    _log->add("User selected FAN -> auto paused");
  } else if (newMode == _cfg->modeDry) {
    setFanByEsp(false);
    _state = ControlState::PAUSED_USER;
    _reason = "Paused: user selected DRY";
    _log->add("User selected DRY -> auto paused");
  }
}

void Thermostat::syncTargetFromRemote(uint32_t now) {
  (void)now;
  const AcState& st = _ac->state();
  // Only adopt remote target changes when AC is running in COOL and no web write is pending
  if (!st.online || !st.rawPower || st.rawMode != _cfg->modeCool) return;
  if (_ac->isTargetPendingWrite() || _pendingTargetSync) return;

  if (st.rawTargetTemp >= TARGET_MIN_C && st.rawTargetTemp <= TARGET_MAX_C &&
      st.rawTargetTemp != _cfg->targetTemp) {
    _cfg->targetTemp = st.rawTargetTemp;
    _store->saveApp(*_cfg);
    resetTempTimers();
    _log->add("Target synced from remote: " + String(_cfg->targetTemp) + " C");
  }
}

void Thermostat::syncBeepSetting(uint32_t now) {
  const AcState& st = _ac->state();
  if (!st.online || !st.rawPower) return;
  if (st.rawBeep == _cfg->beepEnabled) return;
  // Throttle retries if AC temporarily ignores beep writes
  if (now - _lastBeepSyncMs < 5000UL) return;

  _lastBeepSyncMs = now;
  if (_ac->setBeep(_cfg->beepEnabled)) {
    _log->add(String("Synced beep -> ") + (_cfg->beepEnabled ? "ON" : "OFF"));
  }
}

void Thermostat::handleEspModeVerification(uint32_t now) {
  if (!_espVerifyActive) return;
  const AcState& st = _ac->state();
  if (!st.online || !st.rawPower) return;

  if (st.rawMode == _espExpectedMode) {
    _espVerifyActive = false;
    _confirmedMode = _espExpectedMode;
    _candidateMode = _espExpectedMode;
    _candidateSinceMs = 0;

    // Preserve fan level if AC switched fan speed alongside mode change
    if (st.rawFanLevel != _savedFanLevel) {
      if (_ac->setFanLevel(_savedFanLevel)) {
        _log->add("Restored fan level " + String(_savedFanLevel));
      }
    }
    return;
  }

  if (static_cast<int32_t>(now - _espVerifyDeadlineMs) >= 0) {
    _espVerifyActive = false;
    _modeVerifyFailed = true;
    _cfg->autoEnabled = false;
    setFanByEsp(false);
    _store->saveApp(*_cfg);
    _state = ControlState::AUTO_DISABLED;
    _reason = "Mode verification failed (expected " + String(_espExpectedMode) +
              ", got " + String(st.rawMode) + ")";
    _log->add("ERROR: " + _reason + " -> Auto disabled");
  }
}

bool Thermostat::commandEspMode(int targetMode, bool toFan, ControlState nextState, const String& logMsg, uint32_t now) {
  _savedFanLevel = _ac->state().rawFanLevel;

  bool ok = false;
  if (!toFan && _pendingTargetSync) {
    ok = _ac->setModeAndTarget(targetMode, _cfg->targetTemp);
    if (ok) _pendingTargetSync = false;
  } else {
    ok = _ac->setMode(targetMode);
  }

  if (!ok) {
    return false;
  }

  setFanByEsp(toFan);
  if (toFan) {
    _restStartMs = now;
    _lastRawRunning = false;
  } else {
    _coolStartMs = now;
    _lastRawRunning = true;
  }

  _espVerifyActive = true;
  _espExpectedMode = targetMode;
  _espVerifyStartedMs = now;
  _espVerifyDeadlineMs = now + PENDING_CMD_TIMEOUT_MS;
  _confirmedMode = targetMode;
  _candidateMode = targetMode;
  _candidateSinceMs = 0;
  resetTempTimers();

  _state = nextState;
  _log->add(logMsg);
  return true;
}

void Thermostat::handleSensorFaultTransitions(uint32_t now) {
  const bool fault = _sensor->isFault();

  if (fault) {
    resetTempTimers();
    if (_state == ControlState::AUTO_COOL) {
      _state = ControlState::FAULT_COOL_WAIT;
      _reason = "Sensor fault: holding COOL until 3-min min run completes";
      _log->add("Sensor fault while COOL: waiting 3-min protection before FAN");
    } else if (_state == ControlState::AUTO_FAN) {
      _state = ControlState::FAULT_FAN;
      _reason = "Sensor fault: holding FAN safely";
      _log->add("Sensor fault while FAN: holding FAN");
    }

    if (_state == ControlState::FAULT_COOL_WAIT && getRunLockRemainingMs(now) == 0) {
      if (commandEspMode(_cfg->modeFan, true, ControlState::FAULT_FAN,
                         "Sensor fault 3-min protection complete -> switched to FAN", now)) {
        _reason = "Sensor fault: switched to safe FAN";
      }
    }
    return;
  }

  // Sensor is healthy (or recovered after 30s stability)
  if (_state == ControlState::FAULT_COOL_WAIT) {
    _state = ControlState::AUTO_COOL;
    _reason = "Sensor recovered: resumed AUTO_COOL";
    _log->add("Sensor recovered -> resumed AUTO_COOL");
  } else if (_state == ControlState::FAULT_FAN) {
    _state = ControlState::AUTO_FAN;
    _reason = "Sensor recovered: resumed AUTO_FAN";
    _log->add("Sensor recovered -> resumed AUTO_FAN");
  }
  // Note: FAULT_MANUAL intentionally stays in FAULT_MANUAL until user changes mode or turns off AC.
}

void Thermostat::runThermostatHysteresis(uint32_t now) {
  if (!_sensor->hasValidTemp()) return;

  const float roomC = _sensor->tempC();
  const float tFan = fanThreshold();
  const float tCool = coolThreshold();

  if (_state == ControlState::AUTO_COOL) {
    if (roomC < tFan) {
      if (_coldSinceMs == 0) _coldSinceMs = now;
      if (now - _coldSinceMs >= TEMP_CONFIRM_MS) {
        if (getRunLockRemainingMs(now) == 0) {
          if (commandEspMode(_cfg->modeFan, true, ControlState::AUTO_FAN,
                             "Room " + String(roomC, 1) + " C < " + String(tFan, 1) + " C -> switched to FAN", now)) {
            _reason = "Cooling target reached: holding FAN";
          }
        } else {
          _reason = "Cold reached (< " + String(tFan, 1) + " C), waiting 3-min min run";
        }
      } else {
        _reason = "Confirming low temp (< " + String(tFan, 1) + " C)";
      }
    } else {
      _coldSinceMs = 0;
      _reason = "Cooling (FAN at < " + String(tFan, 1) + " C)";
    }
  } else if (_state == ControlState::AUTO_FAN) {
    if (roomC > tCool) {
      if (_warmSinceMs == 0) _warmSinceMs = now;
      if (now - _warmSinceMs >= TEMP_CONFIRM_MS) {
        if (getRestLockRemainingMs(now) == 0) {
          if (commandEspMode(_cfg->modeCool, false, ControlState::AUTO_COOL,
                             "Room " + String(roomC, 1) + " C > " + String(tCool, 1) + " C -> switched to COOL", now)) {
            _reason = "Room warm: switched to COOL";
          }
        } else {
          _reason = "Warm reached (> " + String(tCool, 1) + " C), waiting 3-min min rest";
        }
      } else {
        _reason = "Confirming warm temp (> " + String(tCool, 1) + " C)";
      }
    } else {
      _warmSinceMs = 0;
      _reason = "Resting in FAN (COOL at > " + String(tCool, 1) + " C)";
    }
  }
}

void Thermostat::update(bool pollUpdated) {
  const uint32_t now = millis();
  const AcState& st = _ac->state();

  if (!st.online) {
    if (_state != ControlState::AC_OFFLINE) {
      _state = ControlState::AC_OFFLINE;
      _reason = st.tokenSuspected ? "Offline: token mismatch or rejected" : "AC offline";
      _initializedFromAc = false;
      _espVerifyActive = false;
      _candidateSinceMs = 0;
      resetTempTimers();
    } else if (st.tokenSuspected) {
      _reason = "Offline: token mismatch or rejected";
    }
    return;
  }

  if (pollUpdated) {
    if (!_initializedFromAc) {
      evaluateInitialOrReconnect(now);
    } else {
      updateCompressorTracking(now);

      if (st.power != _lastRawPower) {
        handlePowerTransition(st.power, now);
      }

      if (st.power) {
        handleEspModeVerification(now);

        // User mode debounce only runs when ESP32 is not expecting its own mode write
        if (!_espVerifyActive && !_ac->isModePendingWrite()) {
          if (st.mode != _confirmedMode) {
            if (_candidateSinceMs == 0 || st.mode != _candidateMode) {
              _candidateMode = st.mode;
              _candidateSinceMs = now;
            }
          } else {
            _candidateMode = _confirmedMode;
            _candidateSinceMs = 0;
          }
        }

        syncTargetFromRemote(now);
        syncBeepSetting(now);
      }
    }
  }

  // Continuously check 5s mode debounce completion even between polls
  if (st.power && !_espVerifyActive && !_ac->isModePendingWrite() &&
      _candidateSinceMs != 0 && (now - _candidateSinceMs >= MODE_DEBOUNCE_MS)) {
    _confirmedMode = _candidateMode;
    _candidateSinceMs = 0;
    handleUserModeChange(_confirmedMode, now);
  }

  if (!_cfg->autoEnabled) {
    if (st.power && _state != ControlState::AUTO_DISABLED) {
      _state = ControlState::AUTO_DISABLED;
      _reason = _modeVerifyFailed ? "Auto disabled: mode verification failed" : "Auto control disabled";
    }
    return;
  }

  if (!st.power) return;

  handleSensorFaultTransitions(now);
  runThermostatHysteresis(now);
}

void Thermostat::setAutoEnabled(bool enabled) {
  _cfg->autoEnabled = enabled;
  _modeVerifyFailed = false;
  if (!enabled) {
    setFanByEsp(false);
    _espVerifyActive = false;
    _store->saveApp(*_cfg);
    if (_ac->state().online && _ac->state().power) {
      _state = ControlState::AUTO_DISABLED;
      _reason = "Auto control disabled by user";
    }
    _log->add("Auto control turned OFF");
    return;
  }

  _store->saveApp(*_cfg);
  _log->add("Auto control turned ON");

  const AcState& st = _ac->state();
  if (!st.online) {
    _state = ControlState::AC_OFFLINE;
    _reason = "Waiting for AC connection";
    return;
  }
  if (!st.power) {
    _state = ControlState::AC_OFF;
    _reason = "AC is turned off";
    return;
  }

  const uint32_t now = millis();
  resetTempTimers();
  if (st.mode == _cfg->modeCool) {
    if (_coolStartMs == 0) _coolStartMs = now;
    if (_sensor->isFault()) {
      _state = ControlState::FAULT_COOL_WAIT;
      _reason = "Sensor fault: holding COOL for 3-min protection";
    } else {
      _state = ControlState::AUTO_COOL;
      _reason = "Auto resumed in COOL";
    }
  } else if (st.mode == _cfg->modeFan) {
    // User explicitly enabled Auto while in FAN: ESP32 takes ownership of FAN
    setFanByEsp(true);
    if (_restStartMs == 0) _restStartMs = now;
    if (_sensor->isFault()) {
      _state = ControlState::FAULT_FAN;
      _reason = "Sensor fault: holding FAN";
    } else {
      _state = ControlState::AUTO_FAN;
      _reason = "Auto assumed ownership of FAN";
    }
  } else {
    setFanByEsp(false);
    _state = ControlState::PAUSED_USER;
    _reason = "Paused: AC in DRY mode";
  }
}

bool Thermostat::setTargetFromWeb(int targetC) {
  int clamped = SettingsStore::clampTarget(targetC);
  _cfg->targetTemp = clamped;
  _store->saveApp(*_cfg);
  resetTempTimers();

  const AcState& st = _ac->state();
  if (st.online && st.power && st.mode == _cfg->modeCool) {
    if (_ac->setTargetTemp(clamped)) {
      _pendingTargetSync = false;
      _log->add("Target set via Web -> " + String(clamped) + " C");
      return true;
    }
    return false;
  }

  _pendingTargetSync = true;
  _log->add("Target queued (" + String(clamped) + " C, applies on COOL)");
  return true;
}

void Thermostat::setGapFromWeb(float gapC) {
  _cfg->tempGap = SettingsStore::sanitizeGap(gapC);
  _store->saveApp(*_cfg);
  resetTempTimers();
  _log->add("Gap updated -> " + String(_cfg->tempGap, 1) + " C");
}

bool Thermostat::setBeepFromWeb(bool enabled) {
  _cfg->beepEnabled = enabled;
  _store->saveApp(*_cfg);
  _log->add(String("Beep setting -> ") + (enabled ? "ON" : "OFF"));
  if (_ac->state().online && _ac->state().power) {
    return _ac->setBeep(enabled);
  }
  return true;
}

void Thermostat::notifyWebPowerChanged(bool on) {
  const uint32_t now = millis();
  _ac->requestImmediatePoll();
  if (on && !_lastRawRunning && _ac->state().mode == _cfg->modeCool) {
    _lastRawRunning = true;
  } else if (!on && _lastRawRunning) {
    _restStartMs = now;
    _lastRawRunning = false;
  }
  handlePowerTransition(on, now);
}

void Thermostat::notifyWebModeChanged(int rawMode) {
  const uint32_t now = millis();
  _ac->requestImmediatePoll();
  _espVerifyActive = false;
  _confirmedMode = rawMode;
  _candidateMode = rawMode;
  _candidateSinceMs = 0;

  bool newRunning = _ac->state().power && (rawMode == _cfg->modeCool);
  if (newRunning && !_lastRawRunning) {
    _coolStartMs = now;
  } else if (!newRunning && _lastRawRunning) {
    _restStartMs = now;
  }
  _lastRawRunning = newRunning;

  if (_ac->state().power) {
    handleUserModeChange(rawMode, now);
  }
}
