#pragma once

#include <Arduino.h>

// Load local secrets/network defaults when secrets.h exists (gitignored).
// Copy secrets.h.example to secrets.h to customize values before flashing.
#if defined(__has_include)
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#endif

// Hardware pin configuration (matching existing ESP32-S3 hardware)
#define PIN_ONEWIRE                 21
#define PIN_BOOT_BUTTON             0

// SoftAP configuration for initial setup and recovery (fallback placeholders)
#ifndef AP_SSID
#define AP_SSID                     "MiAC-Setup"
#endif
#ifndef AP_PASS
#define AP_PASS                     "change_this_ap_pass"
#endif
#ifndef AP_IP_ADDR
#define AP_IP_ADDR                  "192.168.4.1"
#endif

// Default static LAN settings (fallback placeholders)
#ifndef DEFAULT_STATIC_IP
#define DEFAULT_STATIC_IP           "192.168.1.50"
#endif
#ifndef DEFAULT_GATEWAY
#define DEFAULT_GATEWAY             "192.168.1.1"
#endif
#ifndef DEFAULT_SUBNET
#define DEFAULT_SUBNET              "255.255.255.0"
#endif
#ifndef DEFAULT_DNS
#define DEFAULT_DNS                 "192.168.1.1"
#endif
#define WEB_SERVER_PORT             80

// Default AC connection settings (Xiaomi New Widetech Pro / nwt.airc.wap31a)
#ifndef DEFAULT_AC_IP
#define DEFAULT_AC_IP               "192.168.1.100"
#endif
#ifndef DEFAULT_AC_PORT
#define DEFAULT_AC_PORT             54321
#endif
#ifndef DEFAULT_AC_TOKEN
#define DEFAULT_AC_TOKEN            "00000000000000000000000000000000"
#endif

// Verified MIoT property coordinates
#define MIOT_POWER_SIID             2
#define MIOT_POWER_PIID             1
#define MIOT_MODE_SIID              2
#define MIOT_MODE_PIID              2
#define MIOT_TARGET_SIID            2
#define MIOT_TARGET_PIID            4
#define MIOT_FAN_SIID               3
#define MIOT_FAN_PIID               2
#define MIOT_ROOM_TEMP_SIID         4
#define MIOT_ROOM_TEMP_PIID         1
#define MIOT_BEEP_SIID              5
#define MIOT_BEEP_PIID              1
#define MIOT_LED_SIID               6
#define MIOT_LED_PIID               1

// Verified MIoT mode values on nwt.airc.wap31a (editable via web /settings)
#define DEFAULT_MODE_COOL           0
#define DEFAULT_MODE_DRY            1
#define DEFAULT_MODE_FAN            2

// Verified MIoT fan levels on nwt.airc.wap31a
#define FAN_LEVEL_LOW               0
#define FAN_LEVEL_HIGH              1

// Thermostat defaults & limits
#define FAN_OFFSET_C                1.0f
#define DEFAULT_TARGET_C            26
#define TARGET_MIN_C                16
#define TARGET_MAX_C                30
#define DEFAULT_GAP_C               4.0f
#define GAP_MIN_C                   2.0f
#define GAP_MAX_C                   7.0f
#define GAP_STEP_C                  0.5f

// Timing constants (milliseconds)
#define MIN_COMPRESSOR_RUN_MS       180000UL  // 3 minutes minimum COOL run
#define MIN_COMPRESSOR_REST_MS      180000UL  // 3 minutes minimum FAN rest
#define STARTUP_LOCK_MS             180000UL  // 3 minutes lock on boot/power-on in COOL
#define TEMP_CONFIRM_MS             10000UL   // 10s continuous threshold breach
#define MODE_DEBOUNCE_MS            5000UL    // 5s stability filter for remote mode cycling
#define PENDING_CMD_TIMEOUT_MS      10000UL   // 10s mode verification window
#define PENDING_PROP_IGNORE_MS      5000UL    // 5s readback suppression after write

#define POLL_INTERVAL_MS            3000UL    // 3s AC poll interval
#define MIIO_TIMEOUT_MS             1500UL    // 1.5s per UDP request
#define MIIO_RETRIES                2
#define MIIO_REHELLO_INTERVAL_MS    300000UL  // 5 minutes stamp refresh
#define OFFLINE_AFTER_FAILS         3

#define SENSOR_READ_INTERVAL_MS     1000UL
#define SENSOR_VALID_MIN_C          0.0f
#define SENSOR_VALID_MAX_C          60.0f
#define SENSOR_FAIL_COUNT           3
#define SENSOR_RECOVER_MS           30000UL   // 30s stable readings before clearing fault

#define WIFI_CONNECT_TIMEOUT_MS     60000UL
#define WIFI_RETRY_INTERVAL_MS      15000UL
#define BOOT_RESET_HOLD_MS          5000UL

#define EVENT_LOG_CAPACITY          30

// Cloud Telemetry & Time synchronization (Turso /v2/pipeline over HTTPS)
#ifndef DEFAULT_TURSO_URL
#define DEFAULT_TURSO_URL           ""
#endif
#ifndef DEFAULT_TURSO_TOKEN
#define DEFAULT_TURSO_TOKEN         ""
#endif
#ifndef DEFAULT_DEVICE_ID
#define DEFAULT_DEVICE_ID           "miac-main"
#endif

#define TELEMETRY_FLUSH_INTERVAL_MS 300000UL  // 5 minutes batch upload
#define TELEMETRY_QUEUE_MAX_LINES   1500      // ~25 hours continuous logging at 1 min/bucket
#define TELEMETRY_QUEUE_FILE        "/tele_q.jsonl"
#define NTP_SERVER_1                "pool.ntp.org"
#define NTP_SERVER_2                "time.google.com"
#define NTP_TIMEZONE_OFFSET_SEC     (7 * 3600)
#define NTP_DAYLIGHT_OFFSET_SEC     0


