#include "json_lite.h"

namespace JsonLite {

namespace {

int skipWhitespace(const String& s, int pos) {
  const int len = s.length();
  while (pos < len) {
    char c = s[pos];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      pos++;
    } else {
      break;
    }
  }
  return pos;
}

// Advance past a JSON string literal starting at the opening quote `pos`.
// Returns the index immediately after the closing quote.
int skipString(const String& s, int pos) {
  const int len = s.length();
  if (pos >= len || s[pos] != '"') return pos;
  pos++;
  while (pos < len) {
    char c = s[pos++];
    if (c == '\\' && pos < len) {
      pos++;
    } else if (c == '"') {
      break;
    }
  }
  return pos;
}

// Advance past any JSON value (object, array, string, or primitive) at `pos`.
// Returns the index immediately after the value.
int skipValue(const String& s, int pos) {
  const int len = s.length();
  pos = skipWhitespace(s, pos);
  if (pos >= len) return pos;

  char c = s[pos];
  if (c == '"') {
    return skipString(s, pos);
  }
  if (c == '{' || c == '[') {
    int depth = 0;
    while (pos < len) {
      char ch = s[pos];
      if (ch == '"') {
        pos = skipString(s, pos);
        continue;
      }
      if (ch == '{' || ch == '[') {
        depth++;
      } else if (ch == '}' || ch == ']') {
        depth--;
        if (depth == 0) {
          return pos + 1;
        }
      }
      pos++;
    }
    return len;
  }

  while (pos < len) {
    char ch = s[pos];
    if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      break;
    }
    pos++;
  }
  return pos;
}

}  // namespace

String escapeString(const String& s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\b') out += "\\b";
    else if (c == '\f') out += "\\f";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (static_cast<uint8_t>(c) < 0x20) out += ' ';
    else out += c;
  }
  return out;
}

String unquoteString(const String& token) {
  String t = token;
  t.trim();
  if (t.length() >= 2 && t[0] == '"' && t[t.length() - 1] == '"') {
    t = t.substring(1, t.length() - 1);
  }
  String out;
  out.reserve(t.length());
  for (size_t i = 0; i < t.length(); i++) {
    if (t[i] == '\\' && i + 1 < t.length()) {
      char n = t[++i];
      if (n == 'n') out += '\n';
      else if (n == 'r') out += '\r';
      else if (n == 't') out += '\t';
      else out += n;
    } else {
      out += t[i];
    }
  }
  return out;
}

bool extractRawValue(const String& objJson, const char* key, String& outRaw) {
  const int len = objJson.length();
  int pos = skipWhitespace(objJson, 0);
  if (pos >= len || objJson[pos] != '{') return false;
  pos++;

  while (pos < len) {
    pos = skipWhitespace(objJson, pos);
    if (pos >= len || objJson[pos] == '}') return false;
    if (objJson[pos] == ',') {
      pos++;
      continue;
    }
    if (objJson[pos] != '"') return false;

    int keyEnd = skipString(objJson, pos);
    String curKey = unquoteString(objJson.substring(pos, keyEnd));
    pos = skipWhitespace(objJson, keyEnd);
    if (pos >= len || objJson[pos] != ':') return false;
    pos = skipWhitespace(objJson, pos + 1);

    int valEnd = skipValue(objJson, pos);
    if (curKey == key) {
      outRaw = objJson.substring(pos, valEnd);
      outRaw.trim();
      return true;
    }
    pos = valEnd;
  }
  return false;
}

bool extractInt(const String& objJson, const char* key, int& outVal) {
  String raw;
  if (!extractRawValue(objJson, key, raw) || raw.length() == 0) return false;
  if (raw == "null") return false;
  outVal = raw.toInt();
  return true;
}

bool extractUInt32(const String& objJson, const char* key, uint32_t& outVal) {
  String raw;
  if (!extractRawValue(objJson, key, raw) || raw.length() == 0) return false;
  outVal = static_cast<uint32_t>(strtoul(raw.c_str(), nullptr, 10));
  return true;
}

int splitArray(const String& arrayJson, String* outItems, int maxItems) {
  const int len = arrayJson.length();
  int pos = skipWhitespace(arrayJson, 0);
  if (pos >= len || arrayJson[pos] != '[') return 0;
  pos++;

  int count = 0;
  while (pos < len && count < maxItems) {
    pos = skipWhitespace(arrayJson, pos);
    if (pos >= len || arrayJson[pos] == ']') break;
    if (arrayJson[pos] == ',') {
      pos++;
      continue;
    }
    int elemEnd = skipValue(arrayJson, pos);
    if (elemEnd <= pos) break;
    String elem = arrayJson.substring(pos, elemEnd);
    elem.trim();
    outItems[count++] = elem;
    pos = elemEnd;
  }
  return count;
}

String formatUptime(uint32_t ms) {
  uint32_t totalSec = ms / 1000UL;
  uint32_t sec = totalSec % 60UL;
  uint32_t min = (totalSec / 60UL) % 60UL;
  uint32_t hr = totalSec / 3600UL;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
           static_cast<unsigned long>(hr),
           static_cast<unsigned long>(min),
           static_cast<unsigned long>(sec));
  return String(buf);
}

}  // namespace JsonLite

