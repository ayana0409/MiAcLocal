#pragma once

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "config.h"
#include "event_log.h"

// Non-blocking DS18B20 temperature reader with consecutive-fail fault detection and 30s recovery hysteresis.
class TempSensor {
public:
  TempSensor();
  void begin(EventLog& log);
  void update();

  bool hasValidTemp() const { return _hasReading && !_fault; }
  bool isFault() const { return _fault; }
  float tempC() const { return _lastValidTemp; }
  float rawTempC() const { return _lastRawTemp; }
  int consecutiveFails() const { return _failCount; }

private:
  static bool isReadingValid(float t);

  OneWire _oneWire;
  DallasTemperature _sensors;
  EventLog* _log = nullptr;

  uint32_t _lastReadMs = 0;
  float _lastValidTemp = -999.0f;
  float _lastRawTemp = -999.0f;
  bool _hasReading = false;
  bool _fault = false;
  int _failCount = 0;
  uint32_t _recoverStartMs = 0;
};

