#include "temp_sensor.h"
#include <math.h>

TempSensor::TempSensor()
  : _oneWire(PIN_ONEWIRE), _sensors(&_oneWire) {}

void TempSensor::begin(EventLog& log) {
  _log = &log;
  _sensors.begin();
  _sensors.setResolution(12);
  _sensors.setWaitForConversion(false);
  _sensors.requestTemperatures();
  // Wait one full conversion interval before attempting the first read
  _lastReadMs = millis();
}

bool TempSensor::isReadingValid(float t) {
  if (isnan(t)) return false;
  // Reject disconnected (-127 C), power-on default (85 C), and out-of-range readings
  if (t < SENSOR_VALID_MIN_C || t > SENSOR_VALID_MAX_C) return false;
  return true;
}

void TempSensor::update() {
  const uint32_t now = millis();
  if (now - _lastReadMs < SENSOR_READ_INTERVAL_MS) return;
  _lastReadMs = now;

  // Re-enumerate OneWire bus if no device was detected previously (e.g. loose cable)
  if (_sensors.getDeviceCount() == 0) {
    _sensors.begin();
    _sensors.setResolution(12);
    _sensors.setWaitForConversion(false);
  }

  float t = _sensors.getTempCByIndex(0);
  _lastRawTemp = t;

  if (isReadingValid(t)) {
    _lastValidTemp = t;
    _hasReading = true;
    _failCount = 0;

    if (_fault) {
      if (_recoverStartMs == 0) {
        _recoverStartMs = now;
      } else if (now - _recoverStartMs >= SENSOR_RECOVER_MS) {
        _fault = false;
        _recoverStartMs = 0;
        _log->add("DS18B20 recovered (" + String(t, 1) + " C stable for 30s)");
      }
    }
  } else {
    _recoverStartMs = 0;
    if (_failCount < SENSOR_FAIL_COUNT) {
      _failCount++;
    }
    if (!_fault && _failCount >= SENSOR_FAIL_COUNT) {
      _fault = true;
      _log->add("DS18B20 fault detected (raw=" + String(t, 1) + " C)");
    }
  }

  // Trigger next asynchronous 12-bit conversion
  _sensors.requestTemperatures();
}

