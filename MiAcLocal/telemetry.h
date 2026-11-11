#pragma once

#include <Arduino.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "config.h"
#include "settings_store.h"
#include "ac_device.h"
#include "event_log.h"

// Status summary exposed to Web UI
struct TelemetryStatus {
  bool enabled = false;
  bool ntpSynced = false;
  int pendingCount = 0;
  int lastSyncAgoSec = -1;  // -1 if never synced
  String lastError = "";
};

// Telemetry manager: aggregates 1-minute AC run buckets and pushes batches to Turso SQLite Cloud
class TelemetryManager {
public:
  TelemetryManager();
  ~TelemetryManager();

  void begin(const TelemetrySettings& settings, EventLog& log);
  void update(bool wifiConnected,
              const AcState& acState,
              bool roomTempValid,
              float roomTempC,
              bool autoEnabled);

  void reconfigure(const TelemetrySettings& newSettings);
  void triggerImmediateFlush();
  void flushNow(); // Blocking flush before reboot

  TelemetryStatus getStatus();
  static bool testConnection(const String& url, const String& token, String& errOut);

private:
  static void uploadTaskTrampoline(void* arg);
  void uploadTaskLoop();

  void initNtp();
  bool checkNtpSync();
  time_t currentEpoch();

  void recordSecond(const AcState& acState,
                    bool roomTempValid,
                    float roomTempC,
                    bool autoEnabled);
  void closeCurrentBucket();
  void enqueueBucketLine(const String& line);
  void retroConvertPendingLines(time_t bootEpoch);

  bool uploadBatch(const String& url, const String& token, int maxLines);
  int countPendingLines();

  TelemetrySettings _settings;
  EventLog* _log = nullptr;

  // NTP state
  bool _ntpInitialized = false;
  bool _ntpSynced = false;
  uint32_t _lastNtpCheckMs = 0;
  time_t _bootEpoch = 0;

  // Session & Accumulator state
  bool _inSession = false;
  time_t _sessionStartUTC = 0;
  uint32_t _sessionBootMs = 0;
  bool _lastPower = false;

  uint32_t _lastSecondTickMs = 0;
  int _accumDurationSec = 0;
  int _accumCoolSec = 0;
  int _accumFanLowSec = 0;
  int _accumFanHighSec = 0;
  int _accumOfflineSec = 0;
  int _accumAutoSec = 0;

  float _roomTempSum = 0.0f;
  int _roomTempCount = 0;
  float _roomTempMin = 999.0f;
  float _roomTempMax = -999.0f;

  float _acTempSum = 0.0f;
  int _acTempCount = 0;
  int _lastTargetTemp = DEFAULT_TARGET_C;

  // LittleFS queue & FreeRTOS background task
  SemaphoreHandle_t _fsMutex = nullptr;
  TaskHandle_t _uploadTaskHandle = nullptr;
  volatile bool _immediateFlushRequested = false;

  uint32_t _lastSyncSuccessMs = 0;
  String _lastError = "";
  int _cachedPendingCount = 0;
  uint32_t _lastCountCheckMs = 0;
};

