#include "ac_device.h"
#include "json_lite.h"

namespace {

const char* POLL_PARAMS_JSON =
    "[{\"did\":\"power\",\"siid\":2,\"piid\":1},"
    "{\"did\":\"mode\",\"siid\":2,\"piid\":2},"
    "{\"did\":\"target_temp\",\"siid\":2,\"piid\":4},"
    "{\"did\":\"fan_level\",\"siid\":3,\"piid\":2},"
    "{\"did\":\"room_temp\",\"siid\":4,\"piid\":1},"
    "{\"did\":\"beep\",\"siid\":5,\"piid\":1},"
    "{\"did\":\"led\",\"siid\":6,\"piid\":1}]";

String buildSingleSetJson(const char* did, int siid, int piid, const String& valueLiteral) {
  String s = "[{\"did\":\"";
  s += did;
  s += "\",\"siid\":";
  s += String(siid);
  s += ",\"piid\":";
  s += String(piid);
  s += ",\"value\":";
  s += valueLiteral;
  s += "}]";
  return s;
}

bool checkSetResponseCodes(const String& resultJson) {
  String items[8];
  int n = JsonLite::splitArray(resultJson, items, 8);
  if (n <= 0) return false;
  for (int i = 0; i < n; i++) {
    int code = -1;
    if (!JsonLite::extractInt(items[i], "code", code) || code != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool AcDevice::begin(const AppSettings& cfg, EventLog& log) {
  _log = &log;
  _lastPollStartMs = millis() - POLL_INTERVAL_MS;
  return _miio.begin(cfg.acIp, DEFAULT_AC_PORT, cfg.acToken);
}

bool AcDevice::reconfigure(const AppSettings& cfg) {
  _state.consecutiveFails = 0;
  _state.tokenSuspected = false;
  _pollInFlight = false;
  return _miio.begin(cfg.acIp, DEFAULT_AC_PORT, cfg.acToken);
}

void AcDevice::requestImmediatePoll() {
  _lastPollStartMs = millis() - POLL_INTERVAL_MS;
}

bool AcDevice::isModePendingWrite() const {
  return _pendMode.isActive(millis());
}

bool AcDevice::isTargetPendingWrite() const {
  return _pendTarget.isActive(millis());
}

bool AcDevice::parseBoolToken(const String& token, bool& outVal) {
  if (token == "true" || token == "1") {
    outVal = true;
    return true;
  }
  if (token == "false" || token == "0") {
    outVal = false;
    return true;
  }
  return false;
}

void AcDevice::startPoll() {
  if (_miio.startRpc("get_properties", POLL_PARAMS_JSON)) {
    _pollInFlight = true;
    _lastPollStartMs = millis();
  }
}

bool AcDevice::update(bool wifiConnected, EventLog& log) {
  if (!wifiConnected) {
    _pollInFlight = false;
    _miio.onWifiDisconnected();
    return false;
  }

  _miio.update();

  if (_pollInFlight) {
    MiioTxStatus st = _miio.status();
    if (st == MiioTxStatus::IN_PROGRESS) {
      return false;
    }

    _pollInFlight = false;
    if (st == MiioTxStatus::DONE_OK) {
      String res = _miio.lastResultJson();
      _miio.resetStatus();
      if (parseGetPropsResponse(res)) {
        bool wasOffline = !_state.online && _state.everPolled;
        _state.online = true;
        _state.everPolled = true;
        _state.consecutiveFails = 0;
        _state.tokenSuspected = false;
        _state.lastSuccessMs = millis();
        _state.lastError = "";
        if (wasOffline) {
          log.add("AC connection restored");
        }
        return true;
      } else {
        _state.lastError = "Failed to parse get_properties response";
      }
    } else {
      bool cryptoErr = _miio.lastErrorWasCrypto();
      bool afterHello = _miio.lastErrorAfterHello();
      _state.lastError = _miio.lastError();
      _miio.resetStatus();
      _state.consecutiveFails++;
      if (cryptoErr || (afterHello && _state.consecutiveFails >= OFFLINE_AFTER_FAILS)) {
        _state.tokenSuspected = true;
      }
      if (_state.online && _state.consecutiveFails >= OFFLINE_AFTER_FAILS) {
        _state.online = false;
        log.add("AC offline: " + _state.lastError);
      }
    }
    return false;
  }

  if (!_miio.isBusy() && (millis() - _lastPollStartMs >= POLL_INTERVAL_MS)) {
    startPoll();
  }
  return false;
}

bool AcDevice::parseGetPropsResponse(const String& resultJson) {
  String items[12];
  int n = JsonLite::splitArray(resultJson, items, 12);
  if (n <= 0) return false;

  const uint32_t now = millis();
  bool gotAny = false;

  for (int i = 0; i < n; i++) {
    int siid = 0, piid = 0, code = -1;
    if (!JsonLite::extractInt(items[i], "siid", siid) ||
        !JsonLite::extractInt(items[i], "piid", piid) ||
        !JsonLite::extractInt(items[i], "code", code)) {
      continue;
    }
    if (code != 0) continue;

    String valRaw;
    if (!JsonLite::extractRawValue(items[i], "value", valRaw)) continue;
    gotAny = true;

    if (siid == MIOT_POWER_SIID && piid == MIOT_POWER_PIID) {
      bool b = false;
      if (parseBoolToken(valRaw, b)) {
        _state.rawPower = b;
        _pendPower.apply(b, _state.power, now);
      }
    } else if (siid == MIOT_MODE_SIID && piid == MIOT_MODE_PIID) {
      int m = valRaw.toInt();
      _state.rawMode = m;
      _pendMode.apply(m, _state.mode, now);
    } else if (siid == MIOT_TARGET_SIID && piid == MIOT_TARGET_PIID) {
      int t = valRaw.toInt();
      _state.rawTargetTemp = t;
      _pendTarget.apply(t, _state.targetTemp, now);
    } else if (siid == MIOT_FAN_SIID && piid == MIOT_FAN_PIID) {
      int f = valRaw.toInt();
      _state.rawFanLevel = f;
      _pendFan.apply(f, _state.fanLevel, now);
    } else if (siid == MIOT_ROOM_TEMP_SIID && piid == MIOT_ROOM_TEMP_PIID) {
      _state.roomTemp = static_cast<int>(roundf(valRaw.toFloat()));
    } else if (siid == MIOT_BEEP_SIID && piid == MIOT_BEEP_PIID) {
      bool b = false;
      if (parseBoolToken(valRaw, b)) {
        _state.rawBeep = b;
        _pendBeep.apply(b, _state.beep, now);
      }
    } else if (siid == MIOT_LED_SIID && piid == MIOT_LED_PIID) {
      bool b = false;
      if (parseBoolToken(valRaw, b)) {
        _pendLed.apply(b, _state.led, now);
      }
    }
  }
  return gotAny;
}

bool AcDevice::sendSetProperties(const String& paramsJson) {
  _pollInFlight = false;  // If executeBlocking waited on poll, ensure clean flag
  String res, err;
  if (!_miio.executeBlocking("set_properties", paramsJson, res, err)) {
    _state.lastError = err;
    if (_log) _log->add("AC set failed: " + err);
    return false;
  }
  if (!checkSetResponseCodes(res)) {
    _state.lastError = "AC rejected set_properties: " + res;
    if (_log) _log->add(_state.lastError);
    return false;
  }
  return true;
}

bool AcDevice::writeIntProp(const char* did, int siid, int piid, int val, int& filteredOut, PendingProp<int>& pending) {
  if (!sendSetProperties(buildSingleSetJson(did, siid, piid, String(val)))) return false;
  filteredOut = val;
  pending.arm(val, millis() + PENDING_PROP_IGNORE_MS);
  return true;
}

bool AcDevice::writeBoolProp(const char* did, int siid, int piid, bool val, bool& filteredOut, PendingProp<bool>& pending) {
  if (!sendSetProperties(buildSingleSetJson(did, siid, piid, val ? "true" : "false"))) return false;
  filteredOut = val;
  pending.arm(val, millis() + PENDING_PROP_IGNORE_MS);
  return true;
}

bool AcDevice::setPower(bool on) {
  return writeBoolProp("power", MIOT_POWER_SIID, MIOT_POWER_PIID, on, _state.power, _pendPower);
}

bool AcDevice::setMode(int rawMode) {
  return writeIntProp("mode", MIOT_MODE_SIID, MIOT_MODE_PIID, rawMode, _state.mode, _pendMode);
}

bool AcDevice::setModeAndTarget(int rawMode, int targetC) {
  String params = "[{\"did\":\"mode\",\"siid\":";
  params += String(MIOT_MODE_SIID);
  params += ",\"piid\":";
  params += String(MIOT_MODE_PIID);
  params += ",\"value\":";
  params += String(rawMode);
  params += "},{\"did\":\"target_temp\",\"siid\":";
  params += String(MIOT_TARGET_SIID);
  params += ",\"piid\":";
  params += String(MIOT_TARGET_PIID);
  params += ",\"value\":";
  params += String(targetC);
  params += "}]";

  if (!sendSetProperties(params)) return false;
  const uint32_t until = millis() + PENDING_PROP_IGNORE_MS;
  _state.mode = rawMode;
  _pendMode.arm(rawMode, until);
  _state.targetTemp = targetC;
  _pendTarget.arm(targetC, until);
  return true;
}

bool AcDevice::setTargetTemp(int targetC) {
  return writeIntProp("target_temp", MIOT_TARGET_SIID, MIOT_TARGET_PIID, targetC, _state.targetTemp, _pendTarget);
}

bool AcDevice::setFanLevel(int level) {
  return writeIntProp("fan_level", MIOT_FAN_SIID, MIOT_FAN_PIID, level, _state.fanLevel, _pendFan);
}

bool AcDevice::setBeep(bool on) {
  return writeBoolProp("beep", MIOT_BEEP_SIID, MIOT_BEEP_PIID, on, _state.beep, _pendBeep);
}

bool AcDevice::setLed(bool on) {
  return writeBoolProp("led", MIOT_LED_SIID, MIOT_LED_PIID, on, _state.led, _pendLed);
}

bool AcDevice::fetchInfoJson(String& outJson, String& outError) {
  _pollInFlight = false;
  return _miio.executeBlocking("miIO.info", "[]", outJson, outError);
}

bool AcDevice::fetchRawPropsJson(String& outJson, String& outError) {
  _pollInFlight = false;
  return _miio.executeBlocking("get_properties", POLL_PARAMS_JSON, outJson, outError);
}

bool AcDevice::setRawProperty(int siid, int piid, const String& valueJson, String& outResp, String& outError) {
  _pollInFlight = false;
  String did = String(siid) + "." + String(piid);
  String params = buildSingleSetJson(did.c_str(), siid, piid, valueJson);
  return _miio.executeBlocking("set_properties", params, outResp, outError);
}
