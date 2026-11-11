#pragma once

#include <Arduino.h>

namespace JsonLite {

// Escape a string so it is safe to embed inside a JSON string literal.
String escapeString(const String& s);

// Unescape a JSON string token (with or without surrounding quotes).
String unquoteString(const String& token);

// Extract the raw JSON value substring for `key` at the top level of a JSON object.
// Handles nested objects, arrays, and quoted strings safely.
bool extractRawValue(const String& objJson, const char* key, String& outRaw);

// Extract integer value for `key` at the top level of a JSON object.
bool extractInt(const String& objJson, const char* key, int& outVal);

// Extract uint32 value for `key` at the top level of a JSON object.
bool extractUInt32(const String& objJson, const char* key, uint32_t& outVal);

// Split a top-level JSON array string `[a, b, ...]` into element substrings.
// Returns the number of elements written to `outItems` (up to `maxItems`).
int splitArray(const String& arrayJson, String* outItems, int maxItems);

// Format an unsigned millisecond duration as "HH:MM:SS".
String formatUptime(uint32_t ms);

}  // namespace JsonLite

