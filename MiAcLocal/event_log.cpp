#include "event_log.h"
#include "json_lite.h"

void EventLog::add(const String& msg) {
  _entries[_head].timestampMs = millis();
  _entries[_head].message = msg;
  _head = (_head + 1) % EVENT_LOG_CAPACITY;
  if (_count < EVENT_LOG_CAPACITY) {
    _count++;
  }
  Serial.printf("[%s] %s\n", JsonLite::formatUptime(millis()).c_str(), msg.c_str());
}

String EventLog::toJsonArray() const {
  String json = "[";
  // Emit newest entries first so the UI stays readable without scrolling
  for (int i = 0; i < _count; i++) {
    int idx = (_head - 1 - i + EVENT_LOG_CAPACITY) % EVENT_LOG_CAPACITY;
    if (i > 0) json += ",";
    json += "{\"t\":\"";
    json += JsonLite::formatUptime(_entries[idx].timestampMs);
    json += "\",\"m\":\"";
    json += JsonLite::escapeString(_entries[idx].message);
    json += "\"}";
  }
  json += "]";
  return json;
}

