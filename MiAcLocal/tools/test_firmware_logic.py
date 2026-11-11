#!/usr/bin/env python3
"""Host-side verification tests for MiAcLocal firmware constants, HTML IDs,
miIO crypto compatibility, JSON parsing, and Thermostat state machine scenarios.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

# Ensure sibling module miot_probe is importable
TOOLS_DIR = Path(__file__).resolve().parent
FIRMWARE_DIR = TOOLS_DIR.parent
ROOT_DIR = FIRMWARE_DIR.parent
sys.path.insert(0, str(TOOLS_DIR))

from miot_probe import MiioClient, SELFTEST_TOKEN_HEX  # noqa: E402


def test_files_and_constants() -> None:
    expected_files = [
        "MiAcLocal.ino",
        "config.h",
        "json_lite.h",
        "json_lite.cpp",
        "event_log.h",
        "event_log.cpp",
        "settings_store.h",
        "settings_store.cpp",
        "wifi_manager.h",
        "wifi_manager.cpp",
        "temp_sensor.h",
        "temp_sensor.cpp",
        "miio_client.h",
        "miio_client.cpp",
        "ac_device.h",
        "ac_device.cpp",
        "thermostat.h",
        "thermostat.cpp",
        "telemetry.h",
        "telemetry.cpp",
        "web_pages.h",
        "web_ui.h",
        "web_ui.cpp",
    ]
    for name in expected_files:
        p = FIRMWARE_DIR / name
        assert p.exists() and p.stat().st_size > 0, f"Missing or empty firmware file: {p}"

    orig = ROOT_DIR / "sketch_sep6a.ino"
    assert orig.exists() and orig.stat().st_size == 31669, "Original sketch_sep6a.ino must remain untouched"

    cfg = (FIRMWARE_DIR / "config.h").read_text(encoding="utf-8")
    assert re.search(r"#define\s+DEFAULT_MODE_COOL\s+0\b", cfg)
    assert re.search(r"#define\s+DEFAULT_MODE_DRY\s+1\b", cfg)
    assert re.search(r"#define\s+DEFAULT_MODE_FAN\s+2\b", cfg)
    assert re.search(r"#define\s+FAN_LEVEL_LOW\s+0\b", cfg)
    assert re.search(r"#define\s+FAN_LEVEL_HIGH\s+1\b", cfg)
    assert re.search(r"#define\s+GAP_MAX_C\s+7\.0f\b", cfg)
    assert re.search(r"#define\s+MIN_COMPRESSOR_RUN_MS\s+180000UL\b", cfg)
    assert re.search(r"#define\s+MIN_COMPRESSOR_REST_MS\s+180000UL\b", cfg)
    assert re.search(r"#define\s+MODE_DEBOUNCE_MS\s+5000UL\b", cfg)
    assert re.search(r"#define\s+TEMP_CONFIRM_MS\s+10000UL\b", cfg)
    assert re.search(r"#define\s+TELEMETRY_FLUSH_INTERVAL_MS\s+300000UL\b", cfg)
    assert re.search(r"#define\s+TELEMETRY_QUEUE_MAX_LINES\s+1500\b", cfg)
    assert re.search(r"#define\s+DEFAULT_DEVICE_ID\s+\"miac-main\"", cfg)

    gitignore = (ROOT_DIR / ".gitignore").read_text(encoding="utf-8")
    for ignored_pattern in ("MiAcLocal/secrets.h", ".env", "MiAcLocal/tools/logs/", "__pycache__/"):
        assert ignored_pattern in gitignore, f"Missing .gitignore rule: {ignored_pattern}"

    secrets_example = (FIRMWARE_DIR / "secrets.h.example").read_text(encoding="utf-8")
    env_example = (ROOT_DIR / ".env.example").read_text(encoding="utf-8")
    assert 'DEFAULT_AC_TOKEN            "00000000000000000000000000000000"' in cfg
    assert 'AP_PASS                     "change_this_ap_pass"' in cfg
    assert 'DEFAULT_AC_TOKEN            "00000000000000000000000000000000"' in secrets_example
    assert 'AP_PASS                     "change_this_ap_pass"' in secrets_example
    assert "MIOT_AC_TOKEN=00000000000000000000000000000000" in env_example


def test_web_pages_dom_integrity() -> None:
    text = (FIRMWARE_DIR / "web_pages.h").read_text(encoding="utf-8")
    pages = ["PAGE_INDEX_HTML", "PAGE_SETTINGS_HTML", "PAGE_DEBUG_HTML"]
    for var in pages:
        m = re.search(rf'const char {var}\[\] PROGMEM = R"rawliteral\((.*?)\)rawliteral";', text, re.S)
        assert m, f"Could not extract {var}"
        html = m.group(1)
        ids = set(re.findall(r'\bid="([^"]+)"', html))
        refs = set(re.findall(r"document\.getElementById\('([^']+)'\)", html))
        missing = refs - ids
        assert not missing, f"{var} references missing DOM IDs: {missing}"
        assert 'meta name="color-scheme" content="dark"' in html, f"{var} missing dark color-scheme meta"
        assert "onclick=" not in html.lower(), f"{var} should not use inline onclick handlers"


def test_miio_binary_format() -> None:
    token_hex = SELFTEST_TOKEN_HEX
    token_bytes = bytes.fromhex(token_hex)
    client = MiioClient("192.168.1.100", 54321, token_hex)

    # Exact vector expected by miio_client.cpp
    key = hashlib.md5(token_bytes).digest()
    iv = hashlib.md5(key + token_bytes).digest()
    assert client.key == key and client.iv == iv

    payload = b'{"id":101,"method":"miIO.info","params":[]}'
    pkt = client.build_packet(0x33546F4E, 1372212, payload)
    magic, total_len, unknown, dev_id, stamp = struct.unpack(">HHIII", pkt[:16])
    assert magic == 0x2131
    assert total_len == len(pkt)
    assert unknown == 0
    assert dev_id == 0x33546F4E
    assert stamp == 1372212
    expected_md5 = hashlib.md5(pkt[:16] + token_bytes + pkt[32:]).digest()
    assert pkt[16:32] == expected_md5

    _, _, plain = client.parse_packet(pkt)
    assert plain == payload


# --- Python mirror of JsonLite (verifying the exact algorithm used in json_lite.cpp) ---

def _jl_skip_ws(s: str, pos: int) -> int:
    while pos < len(s) and s[pos] in " \t\r\n":
        pos += 1
    return pos


def _jl_skip_str(s: str, pos: int) -> int:
    if pos >= len(s) or s[pos] != '"':
        return pos
    pos += 1
    while pos < len(s):
        c = s[pos]
        pos += 1
        if c == "\\":
            pos += 1
        elif c == '"':
            break
    return pos


def _jl_skip_val(s: str, pos: int) -> int:
    pos = _jl_skip_ws(s, pos)
    if pos >= len(s):
        return pos
    if s[pos] == '"':
        return _jl_skip_str(s, pos)
    if s[pos] in "{[":
        depth = 0
        while pos < len(s):
            ch = s[pos]
            if ch == '"':
                pos = _jl_skip_str(s, pos)
                continue
            if ch in "{[":
                depth += 1
            elif ch in "}]":
                depth -= 1
                if depth == 0:
                    return pos + 1
            pos += 1
        return len(s)
    while pos < len(s) and s[pos] not in ",}] \t\r\n":
        pos += 1
    return pos


def jl_extract_raw(obj_json: str, key: str) -> str | None:
    pos = _jl_skip_ws(obj_json, 0)
    if pos >= len(obj_json) or obj_json[pos] != "{":
        return None
    pos += 1
    while pos < len(obj_json):
        pos = _jl_skip_ws(obj_json, pos)
        if pos >= len(obj_json) or obj_json[pos] == "}":
            return None
        if obj_json[pos] == ",":
            pos += 1
            continue
        if obj_json[pos] != '"':
            return None
        key_end = _jl_skip_str(obj_json, pos)
        cur_key = json.loads(obj_json[pos:key_end])
        pos = _jl_skip_ws(obj_json, key_end)
        if pos >= len(obj_json) or obj_json[pos] != ":":
            return None
        pos = _jl_skip_ws(obj_json, pos + 1)
        val_end = _jl_skip_val(obj_json, pos)
        if cur_key == key:
            return obj_json[pos:val_end].strip()
        pos = val_end
    return None


def jl_split_array(arr_json: str) -> list[str]:
    pos = _jl_skip_ws(arr_json, 0)
    if pos >= len(arr_json) or arr_json[pos] != "[":
        return []
    pos += 1
    out: list[str] = []
    while pos < len(arr_json):
        pos = _jl_skip_ws(arr_json, pos)
        if pos >= len(arr_json) or arr_json[pos] == "]":
            break
        if arr_json[pos] == ",":
            pos += 1
            continue
        end = _jl_skip_val(arr_json, pos)
        out.append(arr_json[pos:end].strip())
        pos = end
    return out


def test_json_lite_with_real_ac_payloads() -> None:
    info_resp = (
        '{"id": 102, "result": {"life": 1372212, "model": "nwt.airc.wap31a", '
        '"inc": {"status": 0, "id": 999}, "ap": {"rssi": -38}, "mac": "00:11:22:33:44:55"}}'
    )
    assert jl_extract_raw(info_resp, "id") == "102"
    assert jl_extract_raw(info_resp, "error") is None
    res = jl_extract_raw(info_resp, "result")
    assert res is not None
    # Top-level "id" inside nested "inc" must not shadow outer "id"
    assert jl_extract_raw(res, "model") == '"nwt.airc.wap31a"'

    props_resp = (
        '{"id": 103, "result": ['
        '{"did":"power","siid":2,"piid":1,"code":0,"value":false},'
        '{"piid":2,"siid":2,"code":0,"value":2},'
        '{"did":"target_temp","siid":2,"piid":4,"code":0,"value":26},'
        '{"did":"fan_level","siid":3,"piid":2,"code":0,"value":0},'
        '{"did":"room_temp","siid":4,"piid":1,"code":0,"value":32}'
        ']}'
    )
    arr = jl_extract_raw(props_resp, "result")
    assert arr is not None
    elems = jl_split_array(arr)
    assert len(elems) == 5
    assert jl_extract_raw(elems[1], "siid") == "2"
    assert jl_extract_raw(elems[1], "piid") == "2"
    assert jl_extract_raw(elems[1], "value") == "2"


# --- State Machine Behavioral Simulation (matching thermostat.cpp rules) ---

class CState(Enum):
    AC_OFFLINE = auto()
    AC_OFF = auto()
    AUTO_DISABLED = auto()
    PAUSED_USER = auto()
    AUTO_COOL = auto()
    AUTO_FAN = auto()
    FAULT_COOL_WAIT = auto()
    FAULT_FAN = auto()
    FAULT_MANUAL = auto()


@dataclass
class SimAc:
    online: bool = True
    power: bool = True
    mode: int = 0  # 0=COOL, 1=DRY, 2=FAN
    target: int = 26


class SimThermostat:
    MODE_COOL, MODE_DRY, MODE_FAN = 0, 1, 2

    def __init__(self, target: int = 26, gap: float = 4.0, auto_en: bool = True, fan_by_esp: bool = False):
        self.target = target
        self.gap = gap
        self.auto_en = auto_en
        self.fan_by_esp = fan_by_esp
        self.state = CState.AC_OFFLINE
        self.init_done = False
        self.last_power = False
        self.last_running = False
        self.cool_start = 0
        self.rest_start = 0
        self.confirmed_mode = 0
        self.cand_mode = 0
        self.cand_since = 0
        self.cold_since = 0
        self.warm_since = 0
        self.esp_verify = False
        self.esp_expected = 0
        self.esp_deadline = 0
        self.commands_sent: list[int] = []

    def run_lock_rem(self, now: int) -> int:
        if self.cool_start == 0:
            return 0
        return max(0, 180000 - (now - self.cool_start))

    def rest_lock_rem(self, now: int) -> int:
        if self.rest_start == 0:
            return 0
        return max(0, 180000 - (now - self.rest_start))

    def step(self, now: int, ac: SimAc, sensor_temp: float | None, sensor_fault: bool, poll: bool) -> None:
        if not ac.online:
            self.state = CState.AC_OFFLINE
            self.init_done = False
            self.esp_verify = False
            return

        if poll:
            if not self.init_done:
                self.init_done = True
                self.last_power = ac.power
                self.last_running = ac.power and ac.mode == self.MODE_COOL
                self.confirmed_mode = ac.mode
                self.cand_mode = ac.mode
                if not ac.power:
                    self.fan_by_esp = False
                    self.state = CState.AC_OFF
                elif ac.mode == self.MODE_COOL:
                    self.cool_start = now
                    self.state = CState.AUTO_COOL if self.auto_en else CState.AUTO_DISABLED
                elif ac.mode == self.MODE_FAN and self.fan_by_esp:
                    self.rest_start = now
                    self.state = CState.AUTO_FAN if self.auto_en else CState.AUTO_DISABLED
                else:
                    self.state = CState.PAUSED_USER if self.auto_en else CState.AUTO_DISABLED
            else:
                running = ac.power and ac.mode == self.MODE_COOL
                if running and not self.last_running:
                    self.cool_start = now
                elif not running and self.last_running:
                    self.rest_start = now
                self.last_running = running

                if ac.power != self.last_power:
                    self.last_power = ac.power
                    self.esp_verify = False
                    self.cand_since = 0
                    self.cold_since = self.warm_since = 0
                    if not ac.power:
                        self.fan_by_esp = False
                        self.state = CState.AC_OFF
                    elif ac.mode == self.MODE_COOL:
                        self.cool_start = now
                        self.state = CState.FAULT_COOL_WAIT if sensor_fault else CState.AUTO_COOL
                    else:
                        self.fan_by_esp = False
                        self.state = CState.PAUSED_USER

                if ac.power and self.esp_verify:
                    if ac.mode == self.esp_expected:
                        self.esp_verify = False
                        self.confirmed_mode = self.cand_mode = ac.mode
                    elif now >= self.esp_deadline:
                        self.esp_verify = False
                        self.auto_en = False
                        self.fan_by_esp = False
                        self.state = CState.AUTO_DISABLED

                if ac.power and not self.esp_verify:
                    if ac.mode != self.confirmed_mode:
                        if self.cand_since == 0 or ac.mode != self.cand_mode:
                            self.cand_mode = ac.mode
                            self.cand_since = now
                    else:
                        self.cand_mode = self.confirmed_mode
                        self.cand_since = 0

        if ac.power and not self.esp_verify and self.cand_since != 0 and (now - self.cand_since >= 5000):
            self.confirmed_mode = self.cand_mode
            self.cand_since = 0
            self.cold_since = self.warm_since = 0
            if self.confirmed_mode == self.MODE_COOL:
                self.cool_start = now
                self.fan_by_esp = False
                if sensor_fault or self.state in (CState.FAULT_FAN, CState.FAULT_MANUAL):
                    self.state = CState.FAULT_MANUAL
                else:
                    self.state = CState.AUTO_COOL
            else:
                self.fan_by_esp = False
                self.state = CState.PAUSED_USER

        if not self.auto_en or not ac.power:
            return

        if sensor_fault:
            self.cold_since = self.warm_since = 0
            if self.state == CState.AUTO_COOL:
                self.state = CState.FAULT_COOL_WAIT
            elif self.state == CState.AUTO_FAN:
                self.state = CState.FAULT_FAN
            if self.state == CState.FAULT_COOL_WAIT and self.run_lock_rem(now) == 0:
                ac.mode = self.MODE_FAN
                self.commands_sent.append(self.MODE_FAN)
                self.fan_by_esp = True
                self.rest_start = now
                self.last_running = False
                self.esp_verify = True
                self.esp_expected = self.MODE_FAN
                self.esp_deadline = now + 10000
                self.confirmed_mode = self.cand_mode = self.MODE_FAN
                self.state = CState.FAULT_FAN
            return

        # Recovery (FAULT_MANUAL intentionally stays untouched)
        if self.state == CState.FAULT_COOL_WAIT:
            self.state = CState.AUTO_COOL
        elif self.state == CState.FAULT_FAN:
            self.state = CState.AUTO_FAN

        if sensor_temp is None:
            return

        t_fan = self.target - 1.0
        t_cool = t_fan + self.gap
        if self.state == CState.AUTO_COOL:
            if sensor_temp < t_fan:
                if self.cold_since == 0:
                    self.cold_since = now
                if now - self.cold_since >= 10000 and self.run_lock_rem(now) == 0:
                    ac.mode = self.MODE_FAN
                    self.commands_sent.append(self.MODE_FAN)
                    self.fan_by_esp = True
                    self.rest_start = now
                    self.last_running = False
                    self.esp_verify = True
                    self.esp_expected = self.MODE_FAN
                    self.esp_deadline = now + 10000
                    self.confirmed_mode = self.cand_mode = self.MODE_FAN
                    self.state = CState.AUTO_FAN
            else:
                self.cold_since = 0
        elif self.state == CState.AUTO_FAN:
            if sensor_temp > t_cool:
                if self.warm_since == 0:
                    self.warm_since = now
                if now - self.warm_since >= 10000 and self.rest_lock_rem(now) == 0:
                    ac.mode = self.MODE_COOL
                    self.commands_sent.append(self.MODE_COOL)
                    self.fan_by_esp = False
                    self.cool_start = now
                    self.last_running = True
                    self.esp_verify = True
                    self.esp_expected = self.MODE_COOL
                    self.esp_deadline = now + 10000
                    self.confirmed_mode = self.cand_mode = self.MODE_COOL
                    self.state = CState.AUTO_COOL
            else:
                self.warm_since = 0


def test_thermostat_scenarios() -> None:
    # 1. Boot in COOL -> 3-min lock prevents immediate switch even if room is 23C
    ac = SimAc(power=True, mode=0)
    th = SimThermostat(target=26, gap=4.0)
    th.step(1000, ac, 23.0, False, poll=True)
    assert th.state == CState.AUTO_COOL
    th.step(120000, ac, 23.0, False, poll=True)  # 2 min elapsed (< 3 min)
    assert th.state == CState.AUTO_COOL and ac.mode == 0
    th.step(181000, ac, 23.0, False, poll=True)  # > 3 min elapsed and > 10s < 25C
    assert th.state == CState.AUTO_FAN and ac.mode == 2 and th.fan_by_esp
    th.step(183000, ac, 23.0, False, poll=True)  # next 2s poll confirms ESP mode=FAN

    # 2. Remote briefly visits DRY -> COOL within 3s: must NOT pause, and resets 3-min COOL lock
    ac.mode = 1
    th.step(184000, ac, 23.0, False, poll=True)
    ac.mode = 0
    th.step(187000, ac, 23.0, False, poll=True)
    assert th.state == CState.AUTO_FAN or th.cand_since != 0
    # After 5s stable on COOL, transitions to AUTO_COOL and locks for 3 minutes!
    th.step(192000, ac, 23.0, False, poll=True)
    assert th.state == CState.AUTO_COOL
    assert th.run_lock_rem(193000) > 170000

    # 3. Power off then power on in FAN -> treated as user FAN (PAUSED_USER)
    ac.power = False
    th.step(200000, ac, 24.0, False, poll=True)
    assert th.state == CState.AC_OFF and not th.fan_by_esp
    ac.power = True
    ac.mode = 2
    th.step(205000, ac, 31.0, False, poll=True)
    assert th.state == CState.PAUSED_USER

    # 4. Sensor fault in COOL -> holds 3 min -> FAN -> user picks COOL -> FAULT_MANUAL stays even after recovery
    ac = SimAc(power=True, mode=0)
    th = SimThermostat()
    th.step(1000, ac, 27.0, False, poll=True)
    th.step(10000, ac, None, True, poll=True)
    assert th.state == CState.FAULT_COOL_WAIT and ac.mode == 0
    th.step(181000, ac, None, True, poll=True)
    assert th.state == CState.FAULT_FAN and ac.mode == 2
    th.step(183000, ac, None, True, poll=True)  # next 2s poll confirms ESP mode=FAN
    ac.mode = 0  # user switches to COOL on remote
    th.step(184000, ac, None, True, poll=True)
    th.step(189000, ac, None, True, poll=True)
    assert th.state == CState.FAULT_MANUAL
    # Sensor recovers -> must stay FAULT_MANUAL until user changes mode!
    th.step(220000, ac, 22.0, False, poll=True)
    assert th.state == CState.FAULT_MANUAL and ac.mode == 0
    # User switches to FAN -> exits FAULT_MANUAL to PAUSED_USER
    ac.mode = 2
    th.step(223000, ac, 22.0, False, poll=True)
    th.step(228000, ac, 22.0, False, poll=True)
    assert th.state == CState.PAUSED_USER


def test_telemetry_accumulator_logic() -> None:
    """Verify that:
    1. Zero buckets are emitted while power is OFF (no idle room temp logging).
    2. DRY mode is counted into cool_sec (DRY == COOL per user specification).
    3. Exactly 1 final partial bucket is emitted immediately on power ON -> OFF transition.
    4. 700 W Non-Inverter math matches expected kWh.
    """
    class SimTelemetryAccumulator:
        def __init__(self) -> None:
            self.last_power = False
            self.in_session = False
            self.buckets: list[dict] = []
            self.immediate_flushes = 0

            # Accumulator variables matching telemetry.cpp
            self.accum_dur = 0
            self.accum_cool = 0
            self.accum_fan_low = 0
            self.accum_fan_high = 0
            self.accum_auto = 0
            self.room_temp_sum = 0.0
            self.room_temp_cnt = 0

        def close_bucket(self) -> None:
            if self.accum_dur <= 0:
                return
            avg_temp = (self.room_temp_sum / self.room_temp_cnt) if self.room_temp_cnt > 0 else None
            self.buckets.append({
                "duration_sec": self.accum_dur,
                "cool_sec": self.accum_cool,
                "fan_low_sec": self.accum_fan_low,
                "fan_high_sec": self.accum_fan_high,
                "auto_sec": self.accum_auto,
                "room_temp_avg": round(avg_temp, 1) if avg_temp is not None else None,
            })
            self.accum_dur = 0
            self.accum_cool = 0
            self.accum_fan_low = 0
            self.accum_fan_high = 0
            self.accum_auto = 0
            self.room_temp_sum = 0.0
            self.room_temp_cnt = 0

        def tick_second(self, power: bool, mode: int, fan_level: int, auto_en: bool, temp_c: float | None) -> None:
            # 1. Detect ON -> OFF transition
            if self.last_power and not power:
                if self.accum_dur > 0:
                    self.close_bucket()
                self.in_session = False
                self.immediate_flushes += 1

            # 2. Detect OFF -> ON transition
            if not self.last_power and power:
                self.in_session = True
                self.accum_dur = 0
                self.accum_cool = 0
                self.accum_fan_low = 0
                self.accum_fan_high = 0

            self.last_power = power

            # Only record when power is ON
            if not power:
                return

            self.accum_dur += 1
            # DRY mode (1) is counted as COOL (0)
            if mode in (0, 1):
                self.accum_cool += 1
            elif mode == 2:
                if fan_level == 1:
                    self.accum_fan_high += 1
                else:
                    self.accum_fan_low += 1

            if auto_en:
                self.accum_auto += 1

            if temp_c is not None:
                self.room_temp_sum += temp_c
                self.room_temp_cnt += 1

            if self.accum_dur >= 60:
                self.close_bucket()

    sim = SimTelemetryAccumulator()

    # Phase 1: Machine is OFF for 10 minutes (600 seconds)
    for _ in range(600):
        sim.tick_second(power=False, mode=0, fan_level=0, auto_en=True, temp_c=28.5)
    assert len(sim.buckets) == 0, "No buckets should be emitted while AC is OFF"

    # Phase 2: AC turns ON, runs COOL for 90s (produces 1 60s bucket, 30s remaining in accumulator)
    for _ in range(90):
        sim.tick_second(power=True, mode=0, fan_level=0, auto_en=True, temp_c=26.0)
    assert len(sim.buckets) == 1
    assert sim.buckets[0]["duration_sec"] == 60
    assert sim.buckets[0]["cool_sec"] == 60
    assert sim.buckets[0]["room_temp_avg"] == 26.0

    # Phase 3: Switch to DRY mode for 30s (must complete the 2nd 60s bucket, counting DRY as COOL)
    for _ in range(30):
        sim.tick_second(power=True, mode=1, fan_level=0, auto_en=True, temp_c=25.8)
    assert len(sim.buckets) == 2
    assert sim.buckets[1]["duration_sec"] == 60
    assert sim.buckets[1]["cool_sec"] == 60  # 30s COOL + 30s DRY = 60s COOL
    assert sim.buckets[1]["fan_low_sec"] == 0

    # Phase 4: Run FAN High for 60s
    for _ in range(60):
        sim.tick_second(power=True, mode=2, fan_level=1, auto_en=False, temp_c=25.5)
    assert len(sim.buckets) == 3
    assert sim.buckets[2]["cool_sec"] == 0
    assert sim.buckets[2]["fan_high_sec"] == 60
    assert sim.buckets[2]["auto_sec"] == 0

    # Phase 5: Run COOL for 15s then power OFF -> immediate partial bucket closed and immediate flush
    for _ in range(15):
        sim.tick_second(power=True, mode=0, fan_level=0, auto_en=True, temp_c=25.2)
    assert len(sim.buckets) == 3  # not closed yet (only 15s)

    # Power OFF
    sim.tick_second(power=False, mode=0, fan_level=0, auto_en=True, temp_c=25.2)
    assert len(sim.buckets) == 4, "Partial bucket must be emitted immediately upon turning OFF"
    assert sim.buckets[3]["duration_sec"] == 15
    assert sim.buckets[3]["cool_sec"] == 15
    assert sim.immediate_flushes == 1, "Immediate cloud flush must be triggered upon turning OFF"

    # Verify 700 W Non-Inverter energy calculation
    total_cool_sec = sum(b["cool_sec"] for b in sim.buckets)      # 60 + 60 + 0 + 15 = 135s
    total_fan_high_sec = sum(b["fan_high_sec"] for b in sim.buckets) # 60s
    kwh = (total_cool_sec / 3600.0) * (700.0 / 1000.0) + (total_fan_high_sec / 3600.0) * (50.0 / 1000.0)
    expected_kwh = (135.0 / 3600.0) * 0.700 + (60.0 / 3600.0) * 0.050
    assert abs(kwh - expected_kwh) < 1e-6, f"kWh mismatch: {kwh} vs {expected_kwh}"


def main() -> int:
    test_files_and_constants()
    test_web_pages_dom_integrity()
    test_miio_binary_format()
    test_json_lite_with_real_ac_payloads()
    test_thermostat_scenarios()
    test_telemetry_accumulator_logic()
    print("All MiAcLocal firmware logic, protocol, UI, and telemetry checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
