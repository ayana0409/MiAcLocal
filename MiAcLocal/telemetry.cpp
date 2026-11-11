#include "telemetry.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

namespace {

const size_t MAX_BATCH_SIZE = 30;
const uint32_t NTP_CHECK_INTERVAL_MS = 10000UL;
const uint32_t PENDING_COUNT_CHECK_INTERVAL_MS = 5000UL;

String formatFloatValue(float val) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", val);
  return String(buf);
}

}  // namespace

TelemetryManager::TelemetryManager() {
  _fsMutex = xSemaphoreCreateMutex();
}

TelemetryManager::~TelemetryManager() {
  if (_uploadTaskHandle != nullptr) {
    vTaskDelete(_uploadTaskHandle);
    _uploadTaskHandle = nullptr;
  }
  if (_fsMutex != nullptr) {
    vSemaphoreDelete(_fsMutex);
    _fsMutex = nullptr;
  }
}

void TelemetryManager::begin(const TelemetrySettings& settings, EventLog& log) {
  _settings = settings;
  _log = &log;

  if (!LittleFS.begin(true)) {
    _log->add("Telemetry: LittleFS mount failed");
  } else {
    countPendingLines();
  }

  initNtp();

  // Create background upload task pinned to Core 0 (leaving Core 1 for thermostat and Web UI)
  xTaskCreatePinnedToCore(
    uploadTaskTrampoline,
    "TursoUploadTask",
    8192,
    this,
    1,
    &_uploadTaskHandle,
    0
  );

  _log->add("Telemetry initialized (device=" + _settings.deviceId + ", enabled=" +
            (_settings.enabled ? "YES" : "NO") + ", queue=" + String(_cachedPendingCount) + ")");
}

void TelemetryManager::initNtp() {
  if (_ntpInitialized) return;
  configTime(NTP_TIMEZONE_OFFSET_SEC, NTP_DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
  _ntpInitialized = true;
}

bool TelemetryManager::checkNtpSync() {
  time_t now = time(nullptr);
  // Timestamp > 1700000000 corresponds to dates after Nov 2023
  if (now >= 1700000000) {
    if (!_ntpSynced) {
      _ntpSynced = true;
      _bootEpoch = now - (millis() / 1000UL);
      if (_log) {
        _log->add("Telemetry: NTP synchronized (epoch=" + String(now) + ")");
      }
      // If we had session start recorded before NTP sync, retro-convert
      if (_sessionBootMs > 0 && _sessionStartUTC == 0) {
        _sessionStartUTC = _bootEpoch + (_sessionBootMs / 1000UL);
      }
      retroConvertPendingLines(_bootEpoch);
    }
    return true;
  }
  return false;
}

time_t TelemetryManager::currentEpoch() {
  if (checkNtpSync()) {
    return time(nullptr);
  }
  return 0;
}

void TelemetryManager::reconfigure(const TelemetrySettings& newSettings) {
  if (_fsMutex && xSemaphoreTake(_fsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    _settings = newSettings;
    xSemaphoreGive(_fsMutex);
  } else {
    _settings = newSettings;
  }
  triggerImmediateFlush();
}

void TelemetryManager::triggerImmediateFlush() {
  _immediateFlushRequested = true;
  if (_uploadTaskHandle != nullptr) {
    xTaskNotifyGive(_uploadTaskHandle);
  }
}

void TelemetryManager::update(bool wifiConnected,
                              const AcState& acState,
                              bool roomTempValid,
                              float roomTempC,
                              bool autoEnabled) {
  uint32_t nowMs = millis();

  // Periodic NTP check
  if (wifiConnected && (nowMs - _lastNtpCheckMs >= NTP_CHECK_INTERVAL_MS || !_ntpSynced)) {
    _lastNtpCheckMs = nowMs;
    checkNtpSync();
  }

  // Detect ON -> OFF transition
  if (_lastPower && !acState.power) {
    // Air conditioner just turned OFF: close the active partial bucket immediately
    // and trigger an immediate flush to cloud before user turns off ESP32
    if (_accumDurationSec > 0) {
      closeCurrentBucket();
    }
    _inSession = false;
    _sessionStartUTC = 0;
    _sessionBootMs = 0;
    triggerImmediateFlush();
  }

  // Detect OFF -> ON transition: start new session
  if (!_lastPower && acState.power) {
    _inSession = true;
    _sessionBootMs = nowMs;
    if (_ntpSynced) {
      _sessionStartUTC = time(nullptr);
    } else {
      _sessionStartUTC = 0;
    }
    _accumDurationSec = 0;
    _accumCoolSec = 0;
    _accumFanLowSec = 0;
    _accumFanHighSec = 0;
    _accumOfflineSec = 0;
    _accumAutoSec = 0;
    _roomTempSum = 0.0f;
    _roomTempCount = 0;
    _roomTempMin = 999.0f;
    _roomTempMax = -999.0f;
    _acTempSum = 0.0f;
    _acTempCount = 0;
  }

  _lastPower = acState.power;

  // RULE: Only record telemetry when AC is running (power == true).
  // When power == false, record nothing (no idle logging, no empty buckets).
  if (acState.power) {
    if (_lastSecondTickMs == 0 || nowMs - _lastSecondTickMs >= 1000UL) {
      _lastSecondTickMs = nowMs;
      recordSecond(acState, roomTempValid, roomTempC, autoEnabled);
      if (_accumDurationSec >= 60) {
        closeCurrentBucket();
      }
    }
  } else {
    _lastSecondTickMs = nowMs;
  }

  if (nowMs - _lastCountCheckMs >= PENDING_COUNT_CHECK_INTERVAL_MS) {
    _lastCountCheckMs = nowMs;
    countPendingLines();
  }
}

void TelemetryManager::recordSecond(const AcState& acState,
                                    bool roomTempValid,
                                    float roomTempC,
                                    bool autoEnabled) {
  _accumDurationSec++;

  // DRY mode is counted as COOL mode per user specification
  if (acState.mode == DEFAULT_MODE_COOL || acState.mode == DEFAULT_MODE_DRY) {
    _accumCoolSec++;
  } else if (acState.mode == DEFAULT_MODE_FAN) {
    if (acState.fanLevel == FAN_LEVEL_HIGH) {
      _accumFanHighSec++;
    } else {
      _accumFanLowSec++;
    }
  }

  if (!acState.online) {
    _accumOfflineSec++;
  }

  if (autoEnabled) {
    _accumAutoSec++;
  }

  if (roomTempValid) {
    _roomTempSum += roomTempC;
    _roomTempCount++;
    if (roomTempC < _roomTempMin) _roomTempMin = roomTempC;
    if (roomTempC > _roomTempMax) _roomTempMax = roomTempC;
  }

  if (acState.online && acState.roomTemp > 0) {
    _acTempSum += static_cast<float>(acState.roomTemp);
    _acTempCount++;
  }

  _lastTargetTemp = acState.targetTemp;
}

void TelemetryManager::closeCurrentBucket() {
  if (_accumDurationSec <= 0) return;

  time_t bucketTs = 0;
  time_t sessTs = _sessionStartUTC;

  if (checkNtpSync()) {
    bucketTs = time(nullptr);
    if (sessTs == 0 && _sessionBootMs > 0) {
      sessTs = _bootEpoch + (_sessionBootMs / 1000UL);
      _sessionStartUTC = sessTs;
    }
  } else {
    // Relative negative uptime offset if NTP not yet synced
    bucketTs = -static_cast<time_t>(millis() / 1000UL);
    sessTs = -static_cast<time_t>(_sessionBootMs / 1000UL);
  }

  String roomAvgStr = (_roomTempCount > 0) ? formatFloatValue(_roomTempSum / _roomTempCount) : "";
  String roomMinStr = (_roomTempCount > 0) ? formatFloatValue(_roomTempMin) : "";
  String roomMaxStr = (_roomTempCount > 0) ? formatFloatValue(_roomTempMax) : "";
  String acAvgStr = (_acTempCount > 0) ? formatFloatValue(_acTempSum / _acTempCount) : "";
  String targetStr = String(_lastTargetTemp);

  // CSV format:
  // ts,session,dur,cool,fanLow,fanHigh,offline,roomAvg,roomMin,roomMax,acAvg,target,auto
  String line = String(static_cast<long long>(bucketTs)) + "," +
                String(static_cast<long long>(sessTs)) + "," +
                String(_accumDurationSec) + "," +
                String(_accumCoolSec) + "," +
                String(_accumFanLowSec) + "," +
                String(_accumFanHighSec) + "," +
                String(_accumOfflineSec) + "," +
                roomAvgStr + "," +
                roomMinStr + "," +
                roomMaxStr + "," +
                acAvgStr + "," +
                targetStr + "," +
                String(_accumAutoSec);

  enqueueBucketLine(line);

  // Reset accumulator
  _accumDurationSec = 0;
  _accumCoolSec = 0;
  _accumFanLowSec = 0;
  _accumFanHighSec = 0;
  _accumOfflineSec = 0;
  _accumAutoSec = 0;
  _roomTempSum = 0.0f;
  _roomTempCount = 0;
  _roomTempMin = 999.0f;
  _roomTempMax = -999.0f;
  _acTempSum = 0.0f;
  _acTempCount = 0;
}

void TelemetryManager::enqueueBucketLine(const String& line) {
  if (!_fsMutex || xSemaphoreTake(_fsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return;
  }

  File f = LittleFS.open(TELEMETRY_QUEUE_FILE, "a");
  if (f) {
    f.println(line);
    f.close();
    _cachedPendingCount++;
  }

  // Guard against file growth beyond max lines
  if (_cachedPendingCount > TELEMETRY_QUEUE_MAX_LINES) {
    File src = LittleFS.open(TELEMETRY_QUEUE_FILE, "r");
    File dst = LittleFS.open("/tele_q.tmp", "w");
    if (src && dst) {
      int skipLines = _cachedPendingCount - TELEMETRY_QUEUE_MAX_LINES;
      int cur = 0;
      while (src.available()) {
        String l = src.readStringUntil('\n');
        l.trim();
        if (l.length() == 0) continue;
        cur++;
        if (cur > skipLines) {
          dst.println(l);
        }
      }
      src.close();
      dst.close();
      LittleFS.remove(TELEMETRY_QUEUE_FILE);
      LittleFS.rename("/tele_q.tmp", TELEMETRY_QUEUE_FILE);
      _cachedPendingCount = TELEMETRY_QUEUE_MAX_LINES;
    } else {
      if (src) src.close();
      if (dst) dst.close();
    }
  }

  xSemaphoreGive(_fsMutex);
}

void TelemetryManager::retroConvertPendingLines(time_t bootEpoch) {
  if (!_fsMutex || xSemaphoreTake(_fsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return;
  }

  if (!LittleFS.exists(TELEMETRY_QUEUE_FILE)) {
    xSemaphoreGive(_fsMutex);
    return;
  }

  File src = LittleFS.open(TELEMETRY_QUEUE_FILE, "r");
  if (!src) {
    xSemaphoreGive(_fsMutex);
    return;
  }

  File dst = LittleFS.open("/tele_q.tmp", "w");
  if (!dst) {
    src.close();
    xSemaphoreGive(_fsMutex);
    return;
  }

  bool modified = false;
  int lines = 0;

  while (src.available()) {
    String l = src.readStringUntil('\n');
    l.trim();
    if (l.length() == 0) continue;
    lines++;

    // Check first two CSV tokens
    int firstComma = l.indexOf(',');
    if (firstComma > 0) {
      String tsStr = l.substring(0, firstComma);
      long long rawTs = atoll(tsStr.c_str());

      int secondComma = l.indexOf(',', firstComma + 1);
      if (secondComma > 0) {
        String sessStr = l.substring(firstComma + 1, secondComma);
        long long rawSess = atoll(sessStr.c_str());

        if (rawTs < 0 || rawSess < 0) {
          modified = true;
          if (rawTs < 0) {
            rawTs = static_cast<long long>(bootEpoch) + (-rawTs);
          }
          if (rawSess < 0) {
            rawSess = static_cast<long long>(bootEpoch) + (-rawSess);
          }
          String remainder = l.substring(secondComma);
          l = String(rawTs) + "," + String(rawSess) + remainder;
        }
      }
    }
    dst.println(l);
  }

  src.close();
  dst.close();

  if (modified) {
    LittleFS.remove(TELEMETRY_QUEUE_FILE);
    LittleFS.rename("/tele_q.tmp", TELEMETRY_QUEUE_FILE);
  } else {
    LittleFS.remove("/tele_q.tmp");
  }

  _cachedPendingCount = lines;
  xSemaphoreGive(_fsMutex);
}

int TelemetryManager::countPendingLines() {
  if (!_fsMutex || xSemaphoreTake(_fsMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    return _cachedPendingCount;
  }

  if (!LittleFS.exists(TELEMETRY_QUEUE_FILE)) {
    _cachedPendingCount = 0;
    xSemaphoreGive(_fsMutex);
    return 0;
  }

  File f = LittleFS.open(TELEMETRY_QUEUE_FILE, "r");
  if (!f) {
    xSemaphoreGive(_fsMutex);
    return _cachedPendingCount;
  }

  int count = 0;
  while (f.available()) {
    String l = f.readStringUntil('\n');
    l.trim();
    if (l.length() > 0) count++;
  }
  f.close();
  _cachedPendingCount = count;
  xSemaphoreGive(_fsMutex);
  return count;
}

TelemetryStatus TelemetryManager::getStatus() {
  TelemetryStatus st;
  st.enabled = _settings.enabled;
  st.ntpSynced = _ntpSynced;
  st.pendingCount = _cachedPendingCount;
  st.lastError = _lastError;
  if (_lastSyncSuccessMs > 0) {
    st.lastSyncAgoSec = static_cast<int>((millis() - _lastSyncSuccessMs) / 1000UL);
  } else {
    st.lastSyncAgoSec = -1;
  }
  return st;
}

void TelemetryManager::uploadTaskTrampoline(void* arg) {
  TelemetryManager* self = static_cast<TelemetryManager*>(arg);
  self->uploadTaskLoop();
}

void TelemetryManager::uploadTaskLoop() {
  for (;;) {
    // Wait for TELEMETRY_FLUSH_INTERVAL_MS (5 min) or immediate notification
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TELEMETRY_FLUSH_INTERVAL_MS));
    _immediateFlushRequested = false;

    if (!_settings.enabled || _settings.url.length() == 0 || _settings.token.length() == 0) {
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      continue;
    }

    // Process batches until queue is empty or error occurs
    while (_settings.enabled && WiFi.status() == WL_CONNECTED) {
      int pending = countPendingLines();
      if (pending == 0) break;

      bool ok = uploadBatch(_settings.url, _settings.token, MAX_BATCH_SIZE);
      if (!ok) {
        break; // Stop and retry on next cycle
      }
      vTaskDelay(pdMS_TO_TICKS(100)); // Small yield between batches
    }
  }
}

void TelemetryManager::flushNow() {
  if (!_settings.enabled || _settings.url.length() == 0 || _settings.token.length() == 0) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  // Try to send one batch before shutting down / rebooting
  uploadBatch(_settings.url, _settings.token, MAX_BATCH_SIZE);
}

bool TelemetryManager::uploadBatch(const String& url, const String& token, int maxLines) {
  if (!_fsMutex || xSemaphoreTake(_fsMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    return false;
  }

  if (!LittleFS.exists(TELEMETRY_QUEUE_FILE)) {
    xSemaphoreGive(_fsMutex);
    return true;
  }

  File f = LittleFS.open(TELEMETRY_QUEUE_FILE, "r");
  if (!f) {
    xSemaphoreGive(_fsMutex);
    return false;
  }

  struct LineRecord {
    String line;
    long long ts;
    long long sess;
    int dur;
    int cool;
    int fanLow;
    int fanHigh;
    int offline;
    String roomAvg;
    String roomMin;
    String roomMax;
    String acAvg;
    String target;
    int autoSec;
  };

  std::vector<LineRecord> batch;
  std::vector<String> remaining;

  while (f.available()) {
    String l = f.readStringUntil('\n');
    l.trim();
    if (l.length() == 0) continue;

    if (static_cast<int>(batch.size()) < maxLines) {
      // Parse CSV tokens
      // ts,sess,dur,cool,fanLow,fanHigh,offline,roomAvg,roomMin,roomMax,acAvg,target,auto
      LineRecord rec;
      rec.line = l;

      int idx = 0;
      int start = 0;
      bool valid = true;

      for (int part = 0; part < 13; part++) {
        int comma = l.indexOf(',', start);
        String tok = (comma >= 0) ? l.substring(start, comma) : l.substring(start);
        tok.trim();

        switch (part) {
          case 0: rec.ts = atoll(tok.c_str()); break;
          case 1: rec.sess = atoll(tok.c_str()); break;
          case 2: rec.dur = tok.toInt(); break;
          case 3: rec.cool = tok.toInt(); break;
          case 4: rec.fanLow = tok.toInt(); break;
          case 5: rec.fanHigh = tok.toInt(); break;
          case 6: rec.offline = tok.toInt(); break;
          case 7: rec.roomAvg = tok; break;
          case 8: rec.roomMin = tok; break;
          case 9: rec.roomMax = tok; break;
          case 10: rec.acAvg = tok; break;
          case 11: rec.target = tok; break;
          case 12: rec.autoSec = tok.toInt(); break;
        }

        if (comma < 0) {
          if (part < 12) valid = false;
          break;
        }
        start = comma + 1;
      }

      // If timestamp is still negative (unconverted), skip uploading until NTP syncs
      if (valid && rec.ts > 0) {
        batch.push_back(rec);
      } else {
        remaining.push_back(l);
      }
    } else {
      remaining.push_back(l);
    }
  }
  f.close();

  if (batch.empty()) {
    xSemaphoreGive(_fsMutex);
    return true;
  }

  // Construct Hrana v2 pipeline JSON payload
  String jsonBody = "{\"requests\":[";
  for (size_t i = 0; i < batch.size(); i++) {
    const LineRecord& r = batch[i];
    if (i > 0) jsonBody += ",";

    jsonBody += "{\"type\":\"execute\",\"stmt\":{\"sql\":\""
      "INSERT OR REPLACE INTO ac_buckets ("
      "device_id,ts_utc,session_start_utc,duration_sec,cool_sec,fan_low_sec,fan_high_sec,"
      "offline_sec,room_temp_avg,room_temp_min,room_temp_max,ac_temp_avg,target_temp,auto_sec) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)\",\"args\":[";

    // 0: device_id
    jsonBody += "{\"type\":\"text\",\"value\":\"" + _settings.deviceId + "\"},";
    // 1: ts_utc
    jsonBody += "{\"type\":\"integer\",\"value\":\"" + String(r.ts) + "\"},";
    // 2: session_start_utc
    long long sessVal = (r.sess > 0) ? r.sess : r.ts;
    jsonBody += "{\"type\":\"integer\",\"value\":\"" + String(sessVal) + "\"},";
    // 3: duration_sec
    jsonBody += "{\"type\":\"integer\",\"value\":\"" + String(r.dur) + "\"},";
    // 4: cool_sec
    jsonBody += "{\"type\":\"integer\",\"value\":\"" + String(r.cool) + "\"},";
    // 5: fan_low_sec
    jsonBody += "{\"type\":\"integer\",\"value\":\"" + String(r.fanLow) + "\"},";
    // 6: fan_high_sec
    jsonBody += "{\"type\":\"integer\",\"value\":\"" + String(r.fanHigh) + "\"},";
    // 7: offline_sec
    jsonBody += "{\"type\":\"integer\",\"value\":\"" + String(r.offline) + "\"},";

    // 8: room_temp_avg
    if (r.roomAvg.length() > 0) {
      jsonBody += "{\"type\":\"float\",\"value\":" + r.roomAvg + "},";
    } else {
      jsonBody += "{\"type\":\"null\"},";
    }

    // 9: room_temp_min
    if (r.roomMin.length() > 0) {
      jsonBody += "{\"type\":\"float\",\"value\":" + r.roomMin + "},";
    } else {
      jsonBody += "{\"type\":\"null\"},";
    }

    // 10: room_temp_max
    if (r.roomMax.length() > 0) {
      jsonBody += "{\"type\":\"float\",\"value\":" + r.roomMax + "},";
    } else {
      jsonBody += "{\"type\":\"null\"},";
    }

    // 11: ac_temp_avg
    if (r.acAvg.length() > 0) {
      jsonBody += "{\"type\":\"float\",\"value\":" + r.acAvg + "},";
    } else {
      jsonBody += "{\"type\":\"null\"},";
    }

    // 12: target_temp
    if (r.target.length() > 0) {
      jsonBody += "{\"type\":\"integer\",\"value\":\"" + r.target + "\"},";
    } else {
      jsonBody += "{\"type\":\"null\"},";
    }

    // 13: auto_sec
    jsonBody += "{\"type\":\"integer\",\"value\":\"" + String(r.autoSec) + "\"}";

    jsonBody += "]}}";
  }

  jsonBody += ",{\"type\":\"close\"}]}";

  // HTTP POST execution
  WiFiClientSecure secureClient;
  secureClient.setInsecure(); // Skip certificate hierarchy validation on embedded device
  secureClient.setTimeout(10000);

  HTTPClient http;
  String pipelineUrl = SettingsStore::normalizeTursoUrl(url) + "/v2/pipeline";
  bool postOk = false;

  if (http.begin(secureClient, pipelineUrl)) {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + token);
    http.setTimeout(10000);

    int httpCode = http.POST(jsonBody);
    if (httpCode == HTTP_CODE_OK) {
      String resp = http.getString();
      if (resp.indexOf("\"type\":\"ok\"") >= 0 || resp.indexOf("\"type\":\"execute\"") >= 0) {
        postOk = true;
        _lastSyncSuccessMs = millis();
        _lastError = "";
      } else {
        _lastError = "Turso response: " + resp.substring(0, 80);
      }
    } else {
      _lastError = "HTTP " + String(httpCode) + ": " + HTTPClient::errorToString(httpCode);
    }
    http.end();
  } else {
    _lastError = "Failed to connect to " + pipelineUrl;
  }

  if (postOk) {
    // Write back only remaining lines
    File rew = LittleFS.open(TELEMETRY_QUEUE_FILE, "w");
    if (rew) {
      for (const String& line : remaining) {
        rew.println(line);
      }
      rew.close();
      _cachedPendingCount = static_cast<int>(remaining.size());
    }
    xSemaphoreGive(_fsMutex);
    return true;
  }

  xSemaphoreGive(_fsMutex);
  return false;
}

bool TelemetryManager::testConnection(const String& url, const String& token, String& errOut) {
  if (url.length() == 0 || token.length() == 0) {
    errOut = "URL and Token must not be empty";
    return false;
  }

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  secureClient.setTimeout(10000);

  HTTPClient http;
  String pipelineUrl = SettingsStore::normalizeTursoUrl(url) + "/v2/pipeline";

  if (!http.begin(secureClient, pipelineUrl)) {
    errOut = "Cannot connect to " + pipelineUrl;
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + token);
  http.setTimeout(10000);

  // Send a simple SELECT 1 test query
  String testBody = "{\"requests\":[{\"type\":\"execute\",\"stmt\":{\"sql\":\"SELECT 1\"}},{\"type\":\"close\"}]}";
  int httpCode = http.POST(testBody);

  if (httpCode != HTTP_CODE_OK) {
    errOut = "HTTP " + String(httpCode) + " (" + HTTPClient::errorToString(httpCode) + ")";
    http.end();
    return false;
  }

  String resp = http.getString();
  http.end();

  if (resp.indexOf("\"type\":\"error\"") >= 0) {
    errOut = "Turso error: " + resp.substring(0, 100);
    return false;
  }

  errOut = "Connection OK";
  return true;
}

