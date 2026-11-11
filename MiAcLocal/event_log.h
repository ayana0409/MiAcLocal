#pragma once

#include <Arduino.h>
#include "config.h"

struct LogEntry {
  uint32_t timestampMs = 0;
  String message;
};

// In-memory ring buffer storing recent system events for the Web UI.
class EventLog {
public:
  void add(const String& msg);
  String toJsonArray() const;
  int count() const { return _count; }

private:
  LogEntry _entries[EVENT_LOG_CAPACITY];
  int _head = 0;
  int _count = 0;
};

