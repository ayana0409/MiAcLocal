#include <Arduino.h>
#include "config.h"
#include "event_log.h"
#include "settings_store.h"
#include "wifi_manager.h"
#include "temp_sensor.h"
#include "ac_device.h"
#include "thermostat.h"
#include "telemetry.h"
#include "web_ui.h"

// Double Arduino loopTask stack (16 KB instead of 8 KB) when supported by the ESP32 core
#ifdef SET_LOOP_TASK_STACK_SIZE
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#endif

EventLog eventLog;
SettingsStore settingsStore;
AppSettings appCfg;
WifiSettings wifiCfg;
TelemetrySettings teleCfg;

WifiManager wifiMgr;
TempSensor tempSensor;
AcDevice acDevice;
Thermostat thermostat;
TelemetryManager telemetry;
WebUi webUi;

void setup() {
  Serial.begin(115200);
  delay(200);

  settingsStore.loadApp(appCfg);
  settingsStore.loadWifi(wifiCfg);
  settingsStore.loadTelemetry(teleCfg);
  eventLog.add("MiAcLocal booting (target=" + String(appCfg.targetTemp) + "C, gap=" +
               String(appCfg.tempGap, 1) + "C, auto=" + (appCfg.autoEnabled ? "ON" : "OFF") + ")");

  tempSensor.begin(eventLog);
  wifiMgr.begin(wifiCfg, settingsStore, eventLog);
  acDevice.begin(appCfg, eventLog);
  thermostat.begin(appCfg, settingsStore, acDevice, tempSensor, eventLog);
  telemetry.begin(teleCfg, eventLog);
  webUi.begin(appCfg, wifiCfg, teleCfg, settingsStore, wifiMgr, tempSensor, acDevice, thermostat, telemetry, eventLog);
}

void loop() {
  wifiMgr.update();
  tempSensor.update();
  bool pollUpdated = acDevice.update(wifiMgr.isStaConnected(), eventLog);
  thermostat.update(pollUpdated);
  telemetry.update(wifiMgr.isStaConnected(),
                   acDevice.state(),
                   tempSensor.hasValidTemp(),
                   tempSensor.tempC(),
                   appCfg.autoEnabled);
  webUi.update();
  delay(2);
}

