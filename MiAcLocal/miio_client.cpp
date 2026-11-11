#include "miio_client.h"
#include "json_lite.h"
#include <WiFi.h>
#include <MD5Builder.h>
#include <mbedtls/aes.h>
#include <esp_system.h>
#include <string.h>

namespace {

constexpr uint16_t MIIO_MAGIC = 0x2131;
constexpr size_t MIIO_HEADER_LEN = MiioClient::HEADER_LEN;
constexpr size_t MIIO_MAX_PACKET_LEN = MiioClient::MAX_PACKET_LEN;

void writeBe16(uint8_t* dst, uint16_t val) {
  dst[0] = static_cast<uint8_t>((val >> 8) & 0xFF);
  dst[1] = static_cast<uint8_t>(val & 0xFF);
}

void writeBe32(uint8_t* dst, uint32_t val) {
  dst[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
  dst[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
  dst[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
  dst[3] = static_cast<uint8_t>(val & 0xFF);
}

uint16_t readBe16(const uint8_t* src) {
  return (static_cast<uint16_t>(src[0]) << 8) | static_cast<uint16_t>(src[1]);
}

uint32_t readBe32(const uint8_t* src) {
  return (static_cast<uint32_t>(src[0]) << 24) |
         (static_cast<uint32_t>(src[1]) << 16) |
         (static_cast<uint32_t>(src[2]) << 8) |
         static_cast<uint32_t>(src[3]);
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void drainUdpPacket(WiFiUDP& udp) {
  uint8_t scratch[64];
  while (udp.available() > 0) {
    int r = udp.read(scratch, sizeof(scratch));
    if (r <= 0) break;
  }
}

}  // namespace

bool MiioClient::parseHexToken(const String& hex, uint8_t outToken[16]) {
  if (hex.length() != 32) return false;
  for (int i = 0; i < 16; i++) {
    int hi = hexNibble(hex[i * 2]);
    int lo = hexNibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    outToken[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool MiioClient::begin(const String& ipStr, uint16_t port, const String& tokenHex) {
  IPAddress parsedIp;
  if (!parsedIp.fromString(ipStr)) return false;
  uint8_t parsedTok[16];
  if (!parseHexToken(tokenHex, parsedTok)) return false;

  _acIp = parsedIp;
  _acPort = port;
  memcpy(_token, parsedTok, 16);
  deriveKeyIv();

  _configured = true;
  _msgId = (esp_random() % 8000U) + 100U;
  invalidateSession();
  return true;
}

void MiioClient::deriveKeyIv() {
  MD5Builder md5;
  md5.begin();
  md5.add(_token, 16);
  md5.calculate();
  md5.getBytes(_key);

  md5.begin();
  md5.add(_key, 16);
  md5.add(_token, 16);
  md5.calculate();
  md5.getBytes(_iv);
}

void MiioClient::invalidateSession() {
  _hasSession = false;
  _deviceId = 0;
  _deviceStamp = 0;
  _helloTimeMs = 0;
}

void MiioClient::onWifiDisconnected() {
  if (_udpBound) {
    _udp.stop();
    _udpBound = false;
  }
  invalidateSession();
}

bool MiioClient::ensureUdpBound() {
  if (_udpBound) return true;
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    return false;
  }
  // Use explicit non-zero local ports (Core 3.x NetworkUDP can fail or misbehave with begin(0))
  _udpBound = (_udp.begin(54325) != 0) || (_udp.begin(54326) != 0);
  return _udpBound;
}

uint32_t MiioClient::currentStamp() const {
  if (!_hasSession) return 0;
  uint32_t elapsedSec = (millis() - _helloTimeMs) / 1000UL;
  return _deviceStamp + elapsedSec;
}

void MiioClient::resetStatus() {
  if (_txStatus != MiioTxStatus::IN_PROGRESS) {
    _txStatus = MiioTxStatus::IDLE;
  }
}

bool MiioClient::startRpc(const char* method, const String& paramsJson) {
  if (!_configured || isBusy() || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (!ensureUdpBound()) {
    finishError("Failed to bind local UDP socket", false, false);
    return false;
  }
  _reqMethod = method;
  _reqParamsJson = paramsJson;
  _lastResultJson = "";
  _lastError = "";
  _lastErrCrypto = false;
  _lastErrAfterHello = false;
  _retriesLeft = MIIO_RETRIES;
  _txStatus = MiioTxStatus::IN_PROGRESS;

  // Flush any stale UDP packets before starting a new exchange
  while (_udp.parsePacket() > 0) {
    drainUdpPacket(_udp);
  }

  bool sessionExpired = !_hasSession || (millis() - _helloTimeMs >= MIIO_REHELLO_INTERVAL_MS);
  if (sessionExpired) {
    if (!sendHelloPacket()) {
      finishError("Failed to send hello packet", false, false);
      return false;
    }
  } else {
    if (!sendRpcPacket()) {
      finishError("Failed to send RPC packet", false, true);
      return false;
    }
  }
  return true;
}

bool MiioClient::sendHelloPacket() {
  uint8_t hello[MIIO_HEADER_LEN];
  hello[0] = 0x21;
  hello[1] = 0x31;
  hello[2] = 0x00;
  hello[3] = 0x20;
  memset(hello + 4, 0xFF, MIIO_HEADER_LEN - 4);

  if (!_udp.beginPacket(_acIp, _acPort)) return false;
  _udp.write(hello, MIIO_HEADER_LEN);
  if (!_udp.endPacket()) return false;

  _phase = Phase::WAIT_HELLO;
  _phaseStartMs = millis();
  return true;
}

bool MiioClient::sendRpcPacket() {
  _msgId++;
  _pendingMsgId = _msgId;

  String json = "{\"id\":";
  json += String(_pendingMsgId);
  json += ",\"method\":\"";
  json += _reqMethod;
  json += "\",\"params\":";
  json += _reqParamsJson;
  json += "}";

  const size_t plainLen = json.length();
  const size_t padLen = 16 - (plainLen % 16);
  const size_t encLen = plainLen + padLen;
  if (MIIO_HEADER_LEN + encLen > MIIO_MAX_PACKET_LEN) {
    return false;
  }

  memcpy(_plainBuf, json.c_str(), plainLen);
  memset(_plainBuf + plainLen, static_cast<uint8_t>(padLen), padLen);

  // Copy IV because mbedtls_aes_crypt_cbc modifies it in place
  uint8_t ivWork[16];
  memcpy(ivWork, _iv, 16);

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, _key, 128);
  int rc = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, encLen, ivWork, _plainBuf, _txPktBuf + MIIO_HEADER_LEN);
  mbedtls_aes_free(&ctx);
  if (rc != 0) return false;

  writeBe16(_txPktBuf, MIIO_MAGIC);
  writeBe16(_txPktBuf + 2, static_cast<uint16_t>(MIIO_HEADER_LEN + encLen));
  writeBe32(_txPktBuf + 4, 0);
  writeBe32(_txPktBuf + 8, _deviceId);
  writeBe32(_txPktBuf + 12, currentStamp());

  MD5Builder md5;
  md5.begin();
  md5.add(_txPktBuf, 16);
  md5.add(_token, 16);
  md5.add(_txPktBuf + MIIO_HEADER_LEN, static_cast<uint16_t>(encLen));
  md5.calculate();
  md5.getBytes(_txPktBuf + 16);

  if (!_udp.beginPacket(_acIp, _acPort)) return false;
  _udp.write(_txPktBuf, MIIO_HEADER_LEN + encLen);
  if (!_udp.endPacket()) return false;

  _phase = Phase::WAIT_RPC;
  _phaseStartMs = millis();
  return true;
}

void MiioClient::update() {
  if (_txStatus != MiioTxStatus::IN_PROGRESS) return;

  if (WiFi.status() != WL_CONNECTED) {
    onWifiDisconnected();
    finishError("Wi-Fi disconnected", false, false);
    return;
  }

  handleIncomingPacket();
  if (_txStatus != MiioTxStatus::IN_PROGRESS) return;

  const uint32_t now = millis();
  if (now - _phaseStartMs >= MIIO_TIMEOUT_MS) {
    if (_retriesLeft > 0) {
      _retriesLeft--;
      invalidateSession();
      if (!sendHelloPacket()) {
        finishError("UDP retry send failed", false, false);
      }
    } else {
      bool wasWaitRpc = (_phase == Phase::WAIT_RPC);
      finishError(wasWaitRpc ? "RPC timeout (check token/network)" : "Handshake timeout (AC unreachable)",
                  false, wasWaitRpc);
    }
  }
}

void MiioClient::handleIncomingPacket() {
  while (true) {
    int pktLen = _udp.parsePacket();
    if (pktLen <= 0) break;

    if (_udp.remoteIP() != _acIp || pktLen < static_cast<int>(MIIO_HEADER_LEN) ||
        pktLen > static_cast<int>(MIIO_MAX_PACKET_LEN)) {
      drainUdpPacket(_udp);
      continue;
    }

    int n = _udp.read(_rxBuf, pktLen);
    if (n < static_cast<int>(MIIO_HEADER_LEN)) continue;

    uint16_t magic = readBe16(_rxBuf);
    uint16_t declLen = readBe16(_rxBuf + 2);
    if (magic != MIIO_MAGIC || declLen < MIIO_HEADER_LEN || declLen > static_cast<uint16_t>(n)) {
      continue;
    }

    if (_phase == Phase::WAIT_HELLO) {
      _deviceId = readBe32(_rxBuf + 8);
      _deviceStamp = readBe32(_rxBuf + 12);
      _helloTimeMs = millis();
      _hasSession = true;
      _msgId += 100;
      if (!sendRpcPacket()) {
        finishError("Failed to send RPC after hello", false, true);
      }
      return;
    }

    if (_phase == Phase::WAIT_RPC) {
      if (declLen == MIIO_HEADER_LEN) {
        // Ignore stray hello/empty frames
        continue;
      }

      const size_t encLen = declLen - MIIO_HEADER_LEN;
      if (encLen == 0 || (encLen % 16) != 0) {
        finishError("Invalid encrypted payload length", true, true);
        return;
      }

      uint8_t expectedMd5[16];
      MD5Builder md5;
      md5.begin();
      md5.add(_rxBuf, 16);
      md5.add(_token, 16);
      md5.add(_rxBuf + MIIO_HEADER_LEN, static_cast<uint16_t>(encLen));
      md5.calculate();
      md5.getBytes(expectedMd5);

      if (memcmp(_rxBuf + 16, expectedMd5, 16) != 0) {
        finishError("MD5 checksum mismatch (wrong token)", true, true);
        return;
      }

      uint8_t ivWork[16];
      memcpy(ivWork, _iv, 16);

      mbedtls_aes_context ctx;
      mbedtls_aes_init(&ctx);
      mbedtls_aes_setkey_dec(&ctx, _key, 128);
      int rc = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, encLen, ivWork, _rxBuf + MIIO_HEADER_LEN, _plainBuf);
      mbedtls_aes_free(&ctx);
      if (rc != 0) {
        finishError("AES decrypt failed", true, true);
        return;
      }

      uint8_t pad = _plainBuf[encLen - 1];
      if (pad == 0 || pad > 16 || pad > encLen) {
        finishError("Invalid PKCS7 padding (wrong token)", true, true);
        return;
      }
      for (size_t i = encLen - pad; i < encLen; i++) {
        if (_plainBuf[i] != pad) {
          finishError("Corrupt PKCS7 padding (wrong token)", true, true);
          return;
        }
      }

      size_t textLen = encLen - pad;
      while (textLen > 0 && _plainBuf[textLen - 1] == '\0') {
        textLen--;
      }
      _plainBuf[textLen] = '\0';

      String respJson(reinterpret_cast<const char*>(_plainBuf));
      uint32_t respId = 0;
      if (!JsonLite::extractUInt32(respJson, "id", respId)) {
        continue;
      }
      if (respId != _pendingMsgId) {
        // Stale response from a timed-out attempt
        continue;
      }

      String errObj;
      if (JsonLite::extractRawValue(respJson, "error", errObj)) {
        finishError("AC error: " + errObj, false, true);
        return;
      }

      String resVal;
      if (!JsonLite::extractRawValue(respJson, "result", resVal)) {
        finishError("Missing result field in response", false, true);
        return;
      }

      _lastResultJson = resVal;
      _txStatus = MiioTxStatus::DONE_OK;
      _phase = Phase::NONE;
      return;
    }
  }
}

void MiioClient::finishError(const String& err, bool isCrypto, bool afterHello) {
  _lastError = err;
  _lastErrCrypto = isCrypto;
  _lastErrAfterHello = afterHello;
  _txStatus = MiioTxStatus::DONE_ERROR;
  _phase = Phase::NONE;
}

bool MiioClient::executeBlocking(const char* method, const String& paramsJson, String& outResultJson, String& outError) {
  // Let any active background poll finish cleanly before issuing an explicit command
  uint32_t waitStart = millis();
  while (isBusy() && (millis() - waitStart < (MIIO_TIMEOUT_MS * 2))) {
    update();
    delay(2);
  }
  if (isBusy()) {
    invalidateSession();
    _txStatus = MiioTxStatus::IDLE;
    _phase = Phase::NONE;
  }

  if (!startRpc(method, paramsJson)) {
    outError = _lastError.length() ? _lastError : "Cannot start miIO request";
    return false;
  }

  while (isBusy()) {
    update();
    delay(2);
  }

  bool ok = (_txStatus == MiioTxStatus::DONE_OK);
  if (ok) {
    outResultJson = _lastResultJson;
    outError = "";
  } else {
    outResultJson = "";
    outError = _lastError;
  }
  resetStatus();
  return ok;
}
