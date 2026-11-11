#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>
#include <IPAddress.h>
#include "config.h"

enum class MiioTxStatus : uint8_t {
  IDLE = 0,
  IN_PROGRESS,
  DONE_OK,
  DONE_ERROR
};

// Non-blocking miIO UDP client (AES-128-CBC + MD5 handshake/checksum).
// Compatible with both ESP32 Arduino Core 2.x and 3.x.
class MiioClient {
public:
  static constexpr size_t HEADER_LEN = 32;
  static constexpr size_t MAX_PACKET_LEN = 1536;

  bool begin(const String& ipStr, uint16_t port, const String& tokenHex);
  void invalidateSession();
  void onWifiDisconnected();

  // Start an asynchronous RPC call (runs hello automatically if needed).
  bool startRpc(const char* method, const String& paramsJson);

  // Advance pending hello/RPC state machine; must be called frequently in loop().
  void update();

  // Consume completion status for the last started async RPC.
  MiioTxStatus status() const { return _txStatus; }
  bool isBusy() const { return _txStatus == MiioTxStatus::IN_PROGRESS; }
  const String& lastResultJson() const { return _lastResultJson; }
  const String& lastError() const { return _lastError; }
  bool lastErrorWasCrypto() const { return _lastErrCrypto; }
  bool lastErrorAfterHello() const { return _lastErrAfterHello; }
  void resetStatus();

  // Execute an RPC synchronously (waits for any active background tx first).
  // Intended for explicit Web UI actions.
  bool executeBlocking(const char* method, const String& paramsJson, String& outResultJson, String& outError);

  bool hasValidSession() const { return _hasSession; }
  uint32_t deviceId() const { return _deviceId; }
  uint32_t currentStamp() const;

private:
  enum class Phase : uint8_t {
    NONE = 0,
    WAIT_HELLO,
    WAIT_RPC
  };

  void deriveKeyIv();
  bool ensureUdpBound();
  bool sendHelloPacket();
  bool sendRpcPacket();
  void handleIncomingPacket();
  void finishError(const String& err, bool isCrypto, bool afterHello);
  static bool parseHexToken(const String& hex, uint8_t outToken[16]);

  WiFiUDP _udp;
  bool _udpBound = false;

  // Class-level packet buffers avoid blowing the 8 KB Arduino loopTask stack
  uint8_t _rxBuf[MAX_PACKET_LEN] = {0};
  uint8_t _txPktBuf[MAX_PACKET_LEN] = {0};
  uint8_t _plainBuf[MAX_PACKET_LEN - HEADER_LEN + 1] = {0};

  IPAddress _acIp;
  uint16_t _acPort = DEFAULT_AC_PORT;
  uint8_t _token[16] = {0};
  uint8_t _key[16] = {0};
  uint8_t _iv[16] = {0};
  bool _configured = false;

  bool _hasSession = false;
  uint32_t _deviceId = 0;
  uint32_t _deviceStamp = 0;
  uint32_t _helloTimeMs = 0;
  uint32_t _msgId = 1000;

  MiioTxStatus _txStatus = MiioTxStatus::IDLE;
  Phase _phase = Phase::NONE;
  String _reqMethod;
  String _reqParamsJson;
  uint32_t _pendingMsgId = 0;
  uint32_t _phaseStartMs = 0;
  int _retriesLeft = 0;

  String _lastResultJson;
  String _lastError;
  bool _lastErrCrypto = false;
  bool _lastErrAfterHello = false;
};

