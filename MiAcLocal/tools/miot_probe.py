#!/usr/bin/env python3
"""Standalone local miIO probe tool for Xiaomi New Widetech Pro AC.

Implements the Xiaomi miIO UDP (port 54321) protocol directly using AES-128-CBC
and MD5 without depending on python-miio. Designed to inspect miIO.info, read
and watch MIoT properties in real time while toggling the physical remote,
scan for available siid/piid properties, and optionally test set_properties.
"""

from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import queue
import random
import socket
import struct
import sys
import threading
import time
from typing import Any, Callable

from cryptography.hazmat.primitives import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

SELFTEST_TOKEN_HEX = "0123456789abcdef0123456789abcdef"


def _load_local_env() -> dict[str, str]:
    """Load optional MIOT_* defaults from .env files without extra dependencies."""
    values: dict[str, str] = {}
    tools_dir = Path(__file__).resolve().parent
    candidates = [
        tools_dir.parent.parent / ".env",
        tools_dir / ".env",
    ]
    for env_path in candidates:
        if not env_path.is_file():
            continue
        for raw_line in env_path.read_text(encoding="utf-8").splitlines():
            line = raw_line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, val = line.split("=", 1)
            key = key.strip()
            val = val.strip().strip('"').strip("'")
            if key:
                values[key] = val
    for key in ("MIOT_AC_IP", "MIOT_AC_PORT", "MIOT_AC_TOKEN"):
        if os.environ.get(key):
            values[key] = os.environ[key]
    return values


_LOCAL_ENV = _load_local_env()

# Connection defaults read from environment or local .env (gitignored)
DEFAULT_IP = _LOCAL_ENV.get("MIOT_AC_IP", "192.168.1.100")
DEFAULT_PORT = int(_LOCAL_ENV.get("MIOT_AC_PORT", "54321"))
DEFAULT_TOKEN_HEX = _LOCAL_ENV.get("MIOT_AC_TOKEN", "")

MIIO_MAGIC = 0x2131
HEADER_LEN = 32
HELLO_PACKET = b"\x21\x31\x00\x20" + (b"\xff" * 28)

# Known MIoT property mapping to verify against the actual device
PROPS: list[tuple[str, int, int]] = [
    ("power",       2, 1),
    ("mode",        2, 2),
    ("target_temp", 2, 4),
    ("fan_level",   3, 2),
    ("room_temp",   4, 1),
    ("beep",        5, 1),
    ("led",         6, 1),
]


class MiioError(Exception):
    """Base exception for miIO communication errors."""


class MiioNetworkError(MiioError):
    """Raised when UDP handshake or request times out or fails at socket level."""


class MiioCryptoError(MiioError):
    """Raised when checksum or AES decryption fails (usually wrong token)."""


class MiioProtocolError(MiioError):
    """Raised when the device responds with a malformed frame or JSON error."""


class DualLogger:
    """Thread-safe logger that mirrors timestamped output to stdout and a file."""

    def __init__(self, log_path: Path | None) -> None:
        self.log_path = log_path
        self._lock = threading.Lock()
        self._file = None
        if log_path is not None:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            self._file = log_path.open("w", encoding="utf-8")

    def write(self, message: str, to_console: bool = True) -> None:
        with self._lock:
            if to_console:
                print(message, flush=True)
            if self._file is not None:
                self._file.write(message + "\n")
                self._file.flush()

    def close(self) -> None:
        with self._lock:
            if self._file is not None:
                self._file.close()
                self._file = None


class MiioClient:
    """Minimal miIO UDP client supporting handshake, AES-128-CBC, and RPC calls."""

    def __init__(
        self,
        ip: str,
        port: int,
        token_hex: str,
        timeout: float = 2.0,
        debug_fn: Callable[[str], None] | None = None,
    ) -> None:
        clean_token = token_hex.strip().lower()
        if len(clean_token) != 32:
            raise ValueError(f"Token must be 32 hex characters, got {len(clean_token)}")
        try:
            self.token_bytes = bytes.fromhex(clean_token)
        except ValueError as exc:
            raise ValueError("Token contains non-hex characters") from exc

        self.ip = ip
        self.port = port
        self.timeout = timeout
        self.debug_fn = debug_fn

        self.key, self.iv = self.derive_key_iv(self.token_bytes)
        self.device_id: int | None = None
        self.device_stamp: int = 0
        self.hello_monotonic: float = 0.0
        self._msg_id = random.randint(100, 9000)
        self._sock: socket.socket | None = None

    def _log_debug(self, msg: str) -> None:
        if self.debug_fn is not None:
            self.debug_fn(f"[debug] {msg}")

    def _ensure_socket(self) -> socket.socket:
        if self._sock is None:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        return self._sock

    def close(self) -> None:
        if self._sock is not None:
            self._sock.close()
            self._sock = None

    @staticmethod
    def derive_key_iv(token_bytes: bytes) -> tuple[bytes, bytes]:
        """Derive AES-128 key and IV from the 16-byte device token."""
        key = hashlib.md5(token_bytes).digest()
        iv = hashlib.md5(key + token_bytes).digest()
        return key, iv

    @staticmethod
    def encrypt_payload(key: bytes, iv: bytes, plaintext: bytes) -> bytes:
        """Pad with PKCS#7 (128-bit block) and encrypt using AES-128-CBC."""
        padder = padding.PKCS7(128).padder()
        padded = padder.update(plaintext) + padder.finalize()
        cipher = Cipher(algorithms.AES(key), modes.CBC(iv))
        encryptor = cipher.encryptor()
        return encryptor.update(padded) + encryptor.finalize()

    @staticmethod
    def decrypt_payload(key: bytes, iv: bytes, ciphertext: bytes) -> bytes:
        """Decrypt AES-128-CBC ciphertext and remove PKCS#7 padding."""
        if len(ciphertext) == 0 or len(ciphertext) % 16 != 0:
            raise MiioCryptoError(
                f"Encrypted payload length {len(ciphertext)} is not a multiple of 16"
            )
        cipher = Cipher(algorithms.AES(key), modes.CBC(iv))
        decryptor = cipher.decryptor()
        padded = decryptor.update(ciphertext) + decryptor.finalize()
        unpadder = padding.PKCS7(128).unpadder()
        try:
            return unpadder.update(padded) + unpadder.finalize()
        except ValueError as exc:
            raise MiioCryptoError(
                "Invalid PKCS#7 padding (token may be incorrect)"
            ) from exc

    @staticmethod
    def parse_hello_packet(data: bytes) -> tuple[int, int]:
        """Extract (device_id, stamp) from a 32-byte hello response."""
        if len(data) < HEADER_LEN:
            raise MiioProtocolError(f"Hello response too short: {len(data)} bytes")
        magic, length, _unknown, dev_id, stamp = struct.unpack(">HHIII", data[:16])
        if magic != MIIO_MAGIC:
            raise MiioProtocolError(f"Invalid magic in hello response: 0x{magic:04x}")
        if length < HEADER_LEN:
            raise MiioProtocolError(f"Invalid length in hello response: {length}")
        return dev_id, stamp

    def build_packet(self, device_id: int, stamp: int, payload: bytes) -> bytes:
        """Encrypt payload and wrap it with a signed 32-byte miIO header."""
        encrypted = self.encrypt_payload(self.key, self.iv, payload)
        total_len = HEADER_LEN + len(encrypted)
        prefix = struct.pack(">HHIII", MIIO_MAGIC, total_len, 0, device_id, stamp)
        checksum = hashlib.md5(prefix + self.token_bytes + encrypted).digest()
        return prefix + checksum + encrypted

    def parse_packet(self, data: bytes) -> tuple[int, int, bytes]:
        """Verify MD5 checksum and decrypt a miIO response packet."""
        if len(data) < HEADER_LEN:
            raise MiioProtocolError(f"Response too short: {len(data)} bytes")
        magic, length, _unknown, dev_id, stamp = struct.unpack(">HHIII", data[:16])
        if magic != MIIO_MAGIC:
            raise MiioProtocolError(f"Invalid magic in response: 0x{magic:04x}")
        if length > len(data) or length < HEADER_LEN:
            raise MiioProtocolError(
                f"Invalid packet length {length} (received {len(data)} bytes)"
            )
        if length == HEADER_LEN:
            return dev_id, stamp, b""

        rx_checksum = data[16:HEADER_LEN]
        encrypted = data[HEADER_LEN:length]
        expected_checksum = hashlib.md5(
            data[:16] + self.token_bytes + encrypted
        ).digest()
        if rx_checksum != expected_checksum:
            raise MiioCryptoError(
                "Response MD5 checksum mismatch (token may be incorrect)"
            )
        plaintext = self.decrypt_payload(self.key, self.iv, encrypted)
        return dev_id, stamp, plaintext

    def hello(self, retries: int = 2) -> tuple[int, int]:
        """Perform miIO discovery handshake with the device."""
        sock = self._ensure_socket()
        last_err: Exception | None = None

        for attempt in range(retries + 1):
            try:
                sock.settimeout(self.timeout)
                self._log_debug(
                    f"TX hello ({attempt + 1}/{retries + 1}) -> {self.ip}:{self.port}: "
                    f"{HELLO_PACKET.hex()}"
                )
                sock.sendto(HELLO_PACKET, (self.ip, self.port))
                deadline = time.monotonic() + self.timeout
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise socket.timeout("timed out waiting for hello response")
                    sock.settimeout(remaining)
                    data, addr = sock.recvfrom(4096)
                    if addr[0] != self.ip:
                        continue
                    self._log_debug(f"RX hello ({len(data)} B): {data[:32].hex()}")
                    dev_id, stamp = self.parse_hello_packet(data)
                    self.device_id = dev_id
                    self.device_stamp = stamp
                    self.hello_monotonic = time.monotonic()
                    # Jump message ID after a fresh handshake to avoid stale ID collisions
                    self._msg_id += 100
                    return dev_id, stamp
            except (socket.timeout, OSError) as exc:
                last_err = exc

        raise MiioNetworkError(
            f"Handshake failed with {self.ip}:{self.port} ({last_err}). "
            "Check device IP, Wi-Fi subnet, AP/Client isolation on router, and PC firewall."
        )

    def current_stamp(self) -> int:
        """Compute current device stamp based on elapsed time since last hello."""
        elapsed = max(0, int(time.monotonic() - self.hello_monotonic))
        return (self.device_stamp + elapsed) & 0xFFFFFFFF

    def send(self, method: str, params: Any, retries: int = 2) -> Any:
        """Send an encrypted JSON-RPC call and return its 'result' field."""
        if self.device_id is None:
            self.hello(retries=retries)

        sock = self._ensure_socket()
        last_timeout: Exception | None = None

        for attempt in range(retries + 1):
            if attempt > 0:
                self._log_debug("Re-handshaking before retry...")
                self.hello(retries=1)

            self._msg_id += 1
            req_id = self._msg_id
            payload_obj = {"id": req_id, "method": method, "params": params}
            payload_bytes = json.dumps(payload_obj, separators=(",", ":")).encode("utf-8")

            assert self.device_id is not None
            stamp = self.current_stamp()
            packet = self.build_packet(self.device_id, stamp, payload_bytes)

            self._log_debug(
                f"TX id={req_id} stamp={stamp} ({len(packet)} B): {payload_bytes.decode('utf-8')}"
            )
            try:
                sock.sendto(packet, (self.ip, self.port))
            except OSError as exc:
                last_timeout = exc
                continue

            deadline = time.monotonic() + self.timeout
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    last_timeout = socket.timeout("response timeout")
                    break
                sock.settimeout(remaining)
                try:
                    data, addr = sock.recvfrom(4096)
                except socket.timeout as exc:
                    last_timeout = exc
                    break
                except OSError as exc:
                    last_timeout = exc
                    break

                if addr[0] != self.ip:
                    continue
                if len(data) == HEADER_LEN:
                    # Ignore stray hello/empty frames
                    continue

                _, _, plaintext = self.parse_packet(data)
                if not plaintext:
                    continue

                clean_text = plaintext.rstrip(b"\x00").decode("utf-8", errors="replace").strip()
                self._log_debug(f"RX ({len(data)} B): {clean_text}")

                try:
                    resp = json.loads(clean_text)
                except json.JSONDecodeError as exc:
                    raise MiioProtocolError(f"Invalid JSON in response: {clean_text!r}") from exc

                if resp.get("id") != req_id:
                    self._log_debug(
                        f"Ignoring mismatched response id={resp.get('id')} (expected {req_id})"
                    )
                    continue

                if "error" in resp:
                    raise MiioProtocolError(f"Device returned error: {resp['error']}")
                return resp.get("result")

        raise MiioNetworkError(
            f"Request '{method}' timed out after handshake ({last_timeout}). "
            "Token may be incorrect (devices silently drop packets encrypted with wrong token) "
            "or Wi-Fi connection is unstable."
        )

    def get_properties(self, specs: list[tuple[str, int, int]]) -> list[dict[str, Any]]:
        """Call MIoT get_properties for a list of (did, siid, piid)."""
        params = [{"did": did, "siid": siid, "piid": piid} for did, siid, piid in specs]
        result = self.send("get_properties", params)
        if not isinstance(result, list):
            raise MiioProtocolError(f"Expected list from get_properties, got: {result!r}")
        return result

    def set_properties(self, updates: list[tuple[str, int, int, Any]]) -> list[dict[str, Any]]:
        """Call MIoT set_properties for a list of (did, siid, piid, value)."""
        params = [
            {"did": did, "siid": siid, "piid": piid, "value": val}
            for did, siid, piid, val in updates
        ]
        result = self.send("set_properties", params)
        if not isinstance(result, list):
            raise MiioProtocolError(f"Expected list from set_properties, got: {result!r}")
        return result


def parse_typed_value(raw: str, forced_type: str | None = None) -> Any:
    """Parse CLI string into bool, int, float, or str."""
    lower = raw.strip().lower()
    if forced_type == "bool":
        if lower in ("1", "true", "on", "yes"):
            return True
        if lower in ("0", "false", "off", "no"):
            return False
        raise ValueError(f"Cannot parse {raw!r} as bool")
    if forced_type == "int":
        return int(raw)
    if forced_type == "float":
        return float(raw)
    if forced_type == "str":
        return raw

    if lower == "true":
        return True
    if lower == "false":
        return False
    try:
        return int(raw)
    except ValueError:
        pass
    try:
        return float(raw)
    except ValueError:
        return raw


def index_results(
    specs: list[tuple[str, int, int]], items: list[dict[str, Any]]
) -> dict[str, dict[str, Any]]:
    """Map get_properties response items back to our named property specs."""
    by_coord: dict[tuple[int, int], dict[str, Any]] = {}
    by_did: dict[str, dict[str, Any]] = {}
    for item in items:
        if "siid" in item and "piid" in item:
            by_coord[(int(item["siid"]), int(item["piid"]))] = item
        if "did" in item:
            by_did[str(item["did"])] = item

    mapped: dict[str, dict[str, Any]] = {}
    for name, siid, piid in specs:
        item = by_coord.get((siid, piid)) or by_did.get(name)
        if item is not None:
            mapped[name] = item
    return mapped


def format_val(item: dict[str, Any] | None) -> str:
    """Format a MIoT property response item into a concise display string."""
    if item is None:
        return "<missing>"
    code = item.get("code", -1)
    if code != 0:
        return f"<err code={code}>"
    val = item.get("value")
    return repr(val)


def cmd_info(client: MiioClient, log: DualLogger) -> int:
    """Query and display miIO.info."""
    dev_id, stamp = client.hello()
    log.write(f"Handshake OK: device_id=0x{dev_id:08x} ({dev_id}), stamp={stamp}")
    info = client.send("miIO.info", [])
    log.write("miIO.info:")
    log.write(json.dumps(info, indent=2, ensure_ascii=True))
    return 0


def cmd_read(client: MiioClient, log: DualLogger) -> int:
    """Read all 7 mapped MIoT properties once and print a table."""
    items = client.get_properties(PROPS)
    mapped = index_results(PROPS, items)

    log.write(f"Read at {datetime.now().strftime('%H:%M:%S')}:")
    log.write(f"{'Name':<14} {'siid':>4} {'piid':>4} {'Code':>6} {'Type':<8} {'Value'}")
    log.write("-" * 52)
    for name, siid, piid in PROPS:
        item = mapped.get(name)
        if item is None:
            log.write(f"{name:<14} {siid:>4} {piid:>4} {'-':>6} {'-':<8} <missing>")
            continue
        code = item.get("code", -1)
        val = item.get("value") if code == 0 else None
        val_type = type(val).__name__ if code == 0 else "-"
        val_str = repr(val) if code == 0 else f"<error {code}>"
        log.write(f"{name:<14} {siid:>4} {piid:>4} {code:>6} {val_type:<8} {val_str}")
    return 0


def _stdin_reader(note_queue: queue.Queue[str]) -> None:
    """Read lines from stdin in a daemon thread so user notes do not block polling."""
    while True:
        try:
            line = sys.stdin.readline()
        except OSError:
            break
        if line == "":
            break
        text = line.strip()
        if text:
            note_queue.put(text)


def _flush_notes(note_queue: queue.Queue[str], log: DualLogger) -> None:
    while True:
        try:
            note = note_queue.get_nowait()
        except queue.Empty:
            break
        ts = datetime.now().strftime("%H:%M:%S")
        log.write(f"{ts}  [note] {note}")


def cmd_watch(client: MiioClient, log: DualLogger, interval: float) -> int:
    """Poll MIoT properties continuously and log only value/status changes."""
    log.write(
        f"Watching {client.ip}:{client.port} every {interval:.1f}s. "
        "Type a note + Enter anytime (e.g. 'remote -> FAN'). Press Ctrl+C to stop."
    )

    note_queue: queue.Queue[str] = queue.Queue()
    reader = threading.Thread(target=_stdin_reader, args=(note_queue,), daemon=True)
    reader.start()

    last_snapshot: dict[str, tuple[int, Any]] | None = None
    in_error = False

    try:
        while True:
            _flush_notes(note_queue, log)
            ts = datetime.now().strftime("%H:%M:%S")
            try:
                items = client.get_properties(PROPS)
                mapped = index_results(PROPS, items)
                if in_error:
                    log.write(f"{ts}  [recovered] Connection restored")
                    in_error = False

                current: dict[str, tuple[int, Any]] = {}
                for name, _s, _p in PROPS:
                    item = mapped.get(name)
                    if item is None:
                        current[name] = (-999, None)
                    else:
                        code = int(item.get("code", -1))
                        current[name] = (code, item.get("value") if code == 0 else None)

                if last_snapshot is None:
                    summary = "  ".join(
                        f"{name}={format_val(mapped.get(name))}" for name, _s, _p in PROPS
                    )
                    log.write(f"{ts}  {summary}")
                else:
                    changes: list[str] = []
                    for name, _s, _p in PROPS:
                        old_entry = last_snapshot[name]
                        new_entry = current[name]
                        if old_entry != new_entry:
                            old_s = repr(old_entry[1]) if old_entry[0] == 0 else f"<err {old_entry[0]}>"
                            new_s = repr(new_entry[1]) if new_entry[0] == 0 else f"<err {new_entry[0]}>"
                            changes.append(f"{name}: {old_s} -> {new_s}")
                    if changes:
                        log.write(f"{ts}  " + " | ".join(changes))

                last_snapshot = current
            except MiioError as exc:
                if not in_error:
                    log.write(f"{ts}  [error] {exc}")
                    in_error = True

            # Sleep in short increments so queued notes flush promptly
            steps = max(1, int(interval / 0.2))
            step_sleep = interval / steps
            for _ in range(steps):
                _flush_notes(note_queue, log)
                time.sleep(step_sleep)
    except KeyboardInterrupt:
        _flush_notes(note_queue, log)
        log.write(f"{datetime.now().strftime('%H:%M:%S')}  Watch stopped by user.")
        return 0


def cmd_scan(
    client: MiioClient,
    log: DualLogger,
    siid_start: int,
    siid_end: int,
    piid_start: int,
    piid_end: int,
) -> int:
    """Scan a range of siid/piid coordinates and print properties returning code 0."""
    log.write(
        f"Scanning siid {siid_start}..{siid_end}, piid {piid_start}..{piid_end} "
        "(batch size 6)..."
    )
    found: list[dict[str, Any]] = []
    batch: list[tuple[str, int, int]] = []

    def record_items(items: list[dict[str, Any]]) -> None:
        for item in items:
            if item.get("code") == 0:
                found.append(item)
                siid = item.get("siid")
                piid = item.get("piid")
                val = item.get("value")
                log.write(
                    f"  siid={siid:<2} piid={piid:<2} type={type(val).__name__:<6} value={val!r}"
                )

    def flush_batch() -> None:
        if not batch:
            return
        try:
            record_items(client.get_properties(batch))
        except MiioProtocolError:
            # Some firmwares reject an entire batch if one coordinate is invalid; fall back to 1-by-1
            for spec in batch:
                try:
                    record_items(client.get_properties([spec]))
                except MiioError:
                    pass
                time.sleep(0.05)
        except MiioError as exc:
            log.write(f"Batch {batch[0][1]}.{batch[0][2]}.. failed: {exc}")
        batch.clear()
        time.sleep(0.1)

    for siid in range(siid_start, siid_end + 1):
        for piid in range(piid_start, piid_end + 1):
            batch.append((f"{siid}.{piid}", siid, piid))
            if len(batch) >= 6:
                flush_batch()
    flush_batch()

    log.write(f"Scan complete: {len(found)} readable properties found.")
    return 0


def cmd_set(
    client: MiioClient,
    log: DualLogger,
    siid: int,
    piid: int,
    raw_value: str,
    forced_type: str | None,
    assume_yes: bool,
) -> int:
    """Write a single property after explicit confirmation and read it back."""
    val = parse_typed_value(raw_value, forced_type)
    did = f"{siid}.{piid}"
    preview = json.dumps(
        {"did": did, "siid": siid, "piid": piid, "value": val}, ensure_ascii=True
    )
    print(f"About to send set_properties: {preview} (type={type(val).__name__})")

    if not assume_yes:
        reply = input("Confirm sending this command to the AC? [y/N]: ").strip().lower()
        if reply not in ("y", "yes"):
            log.write("Set cancelled by user.")
            return 0

    res = client.set_properties([(did, siid, piid, val)])
    log.write(f"set_properties response: {json.dumps(res, ensure_ascii=True)}")

    time.sleep(0.5)
    readback = client.get_properties([(did, siid, piid)])
    log.write(f"Readback after 0.5s: {json.dumps(readback, ensure_ascii=True)}")
    return 0


def run_selftest() -> int:
    """Run offline crypto, framing, and parser tests without network access."""
    token_hex = SELFTEST_TOKEN_HEX
    token_bytes = bytes.fromhex(token_hex)
    client = MiioClient("192.168.1.100", DEFAULT_PORT, token_hex)

    # 1. Key/IV deterministic MD5 derivation
    expected_key = hashlib.md5(token_bytes).digest()
    expected_iv = hashlib.md5(expected_key + token_bytes).digest()
    assert client.key == expected_key
    assert client.iv == expected_iv

    # 2. Hello frame format and parsing
    assert len(HELLO_PACKET) == 32
    assert HELLO_PACKET[:4] == b"\x21\x31\x00\x20"
    fake_hello_resp = struct.pack(">HHIII", MIIO_MAGIC, 32, 0, 0x12345678, 4242) + (b"\xff" * 16)
    dev_id, stamp = MiioClient.parse_hello_packet(fake_hello_resp)
    assert dev_id == 0x12345678 and stamp == 4242

    # 3. Encrypt -> frame -> verify MD5 -> decrypt roundtrip (including trailing NUL handling)
    sample_obj = {"id": 1001, "method": "get_properties", "params": [{"did": "mode", "siid": 2, "piid": 2}]}
    sample_bytes = json.dumps(sample_obj, separators=(",", ":")).encode("utf-8") + b"\x00"
    pkt = client.build_packet(dev_id, stamp, sample_bytes)
    assert len(pkt) > 32 and len(pkt) % 16 == 0
    rx_dev, rx_stamp, rx_plain = client.parse_packet(pkt)
    assert rx_dev == dev_id and rx_stamp == stamp
    decoded = json.loads(rx_plain.rstrip(b"\x00").decode("utf-8"))
    assert decoded == sample_obj

    # 4. Tampered checksum must raise MiioCryptoError
    tampered = bytearray(pkt)
    tampered[20] ^= 0xFF
    try:
        client.parse_packet(bytes(tampered))
        raise AssertionError("Expected MiioCryptoError on tampered checksum")
    except MiioCryptoError:
        pass

    # 5. Wrong token must raise MiioCryptoError
    other_client = MiioClient(DEFAULT_IP, DEFAULT_PORT, "00112233445566778899aabbccddeeff")
    try:
        other_client.parse_packet(pkt)
        raise AssertionError("Expected MiioCryptoError on wrong token")
    except MiioCryptoError:
        pass

    # 6. CLI value parser checks
    assert parse_typed_value("true") is True
    assert parse_typed_value("FALSE") is False
    assert parse_typed_value("26") == 26 and isinstance(parse_typed_value("26"), int)
    assert parse_typed_value("26.5") == 26.5 and isinstance(parse_typed_value("26.5"), float)
    assert parse_typed_value("1", "bool") is True

    print("All offline miIO selftests passed.")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Local miIO probe tool for Xiaomi New Widetech Pro AC"
    )
    parser.add_argument("--ip", default=DEFAULT_IP, help=f"AC IP address (default: {DEFAULT_IP})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"UDP port (default: {DEFAULT_PORT})")
    parser.add_argument(
        "--token",
        default=DEFAULT_TOKEN_HEX,
        help="32-char hex miIO token (or set MIOT_AC_TOKEN in .env)",
    )
    parser.add_argument("--timeout", type=float, default=2.0, help="UDP timeout in seconds (default: 2.0)")
    parser.add_argument("--debug", action="store_true", help="Print raw hex headers and JSON payloads")
    parser.add_argument("--no-log", action="store_true", help="Disable writing log file under tools/logs/")

    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("info", help="Query miIO.info (model, firmware, MAC)")
    sub.add_parser("read", help="Read the 7 mapped MIoT properties once")

    watch_p = sub.add_parser("watch", help="Poll properties continuously and log changes")
    watch_p.add_argument("--interval", type=float, default=2.0, help="Poll interval in seconds (default: 2.0)")

    scan_p = sub.add_parser("scan", help="Scan siid/piid combinations for readable properties")
    scan_p.add_argument("--siid-start", type=int, default=2)
    scan_p.add_argument("--siid-end", type=int, default=8)
    scan_p.add_argument("--piid-start", type=int, default=1)
    scan_p.add_argument("--piid-end", type=int, default=12)

    set_p = sub.add_parser("set", help="Send set_properties for one property (with confirmation)")
    set_p.add_argument("siid", type=int)
    set_p.add_argument("piid", type=int)
    set_p.add_argument("value", type=str)
    set_p.add_argument("--type", choices=("bool", "int", "float", "str"), default=None)
    set_p.add_argument("-y", "--yes", action="store_true", help="Skip confirmation prompt")

    sub.add_parser("selftest", help="Run offline crypto and packet framing tests")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.command == "selftest":
        return run_selftest()

    if not args.token:
        print(
            "ERROR: Missing 32-char hex miIO token. Pass --token or set MIOT_AC_TOKEN in .env.",
            file=sys.stderr,
        )
        return 2

    log_path: Path | None = None
    if not args.no_log:
        logs_dir = Path(__file__).resolve().parent / "logs"
        ts_name = datetime.now().strftime("%Y%m%d_%H%M%S")
        log_path = logs_dir / f"probe_{ts_name}_{args.command}.log"

    log = DualLogger(log_path)
    debug_cb = (lambda msg: log.write(msg)) if args.debug else None
    client: MiioClient | None = None

    try:
        client = MiioClient(
            ip=args.ip,
            port=args.port,
            token_hex=args.token,
            timeout=args.timeout,
            debug_fn=debug_cb,
        )
        if args.command == "info":
            return cmd_info(client, log)
        if args.command == "read":
            return cmd_read(client, log)
        if args.command == "watch":
            return cmd_watch(client, log, args.interval)
        if args.command == "scan":
            return cmd_scan(
                client, log, args.siid_start, args.siid_end, args.piid_start, args.piid_end
            )
        if args.command == "set":
            return cmd_set(client, log, args.siid, args.piid, args.value, args.type, args.yes)
        return 1
    except (MiioError, ValueError) as exc:
        log.write(f"ERROR: {exc}")
        return 2
    finally:
        if client is not None:
            client.close()
        log.close()
        if log_path is not None and log_path.exists():
            print(f"Log saved to: {log_path}")


if __name__ == "__main__":
    sys.exit(main())
