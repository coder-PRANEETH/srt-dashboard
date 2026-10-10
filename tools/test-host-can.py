#!/usr/bin/env python3
"""Exercise the actual sketch's CAN decoder, GPS JSON, and log on a desktop.

Requires Python 3 and a C++17 compiler (g++ by default, override with CXX).
This extracts the current .ino source rather than maintaining a second copy of
its logic. Only the CAN queue, WebServer response, and Arduino String are
mocked. ESP32 unsigned long is 32 bits; desktop source adapts that type and its
printf format so timestamp overflow behaves like the board.

Run from any directory: python3 tools/test-host-can.py
Hardware initialization, SPI, UART, WiFi, and LittleFS need a real board or a
separate Arduino compile; this test does not simulate them.
"""

import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def section(source, name):
    match = re.search(r"/\*\s*-+\s+" + re.escape(name) + r"\s+-+\s*\*/", source)
    if not match:
        raise RuntimeError(f"Sketch section missing: {name}")
    return match


def integer_packer(source):
    # These actual sender helpers contain no nested braces or string literals.
    return "\n".join(
        re.search(r"void " + name + r"\([^)]*\)\s*\{[^}]*\}", source).group(0)
        for name in ("put32", "put16")
    )


MOCKS = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>
using INT8U = uint8_t;
using INT32U = uint32_t;
constexpr int CAN_OK = 0, CAN_MSGAVAIL = 1;
uint32_t clockMs = 0;
uint32_t millis() { return clockMs; }

class String : public std::string {
public:
  using std::string::string;
  using std::string::operator=;
  String() = default;
  String(const std::string &s) : std::string(s) {}
  template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
  String(T n) : std::string(std::to_string(n)) {}
};

struct Frame { uint32_t id; uint8_t len; std::array<uint8_t, 8> bytes; };
struct MockCAN {
  std::deque<Frame> frames;
  bool failRead = false;
  int checkReceive() { return frames.empty() ? 0 : CAN_MSGAVAIL; }
  int readMsgBuf(INT32U *id, INT8U *len, INT8U *bytes) {
    if (failRead) return 2;
    Frame frame = frames.front();
    frames.pop_front();
    *id = frame.id;
    *len = frame.len;
    memcpy(bytes, frame.bytes.data(), 8);
    return CAN_OK;
  }
} CAN;

struct MockServer {
  std::string response;
  void sendHeader(const char *, const char *) {}
  void send(int status, const char *mime, const std::string &body) {
    assert(status == 200 && std::string(mime) == "application/json");
    response = body;
  }
} server;
"""


CASES = r"""
void reset(uint32_t now = 0) {
  clockMs = now;
  CAN.frames.clear(); CAN.failRead = false;
  canReady = true; canFrames = 0;
  rxLat = rxLon = 0; rxSpeed = rxHdop = 0; rxCourse = 0xFFFF;
  rxSats = rxFix = 0; rxBeat = 0;
  gpsAt = gpsInfoAt = 0; seenPosition = seenInfo = false;
  memset(canLog, 0, sizeof(canLog)); canLogNext = canLogCount = 0;
}
void queue(uint32_t id, const std::array<uint8_t, 8> &bytes, uint8_t len = 8) {
  CAN.frames.push_back({id, len, bytes});
}
void position(int32_t lat = -338688000, int32_t lon = -706693000) {
  std::array<uint8_t, 8> bytes{};
  put32(bytes.data(), lat); put32(bytes.data() + 4, lon);
  queue(CAN_ID_POS, bytes); readCan();
}
void info(uint8_t fix = 1, uint16_t course = 1234, uint16_t hdop = 87) {
  std::array<uint8_t, 8> bytes{};
  put16(bytes.data(), 456); put16(bytes.data() + 2, course);
  bytes[4] = 9; bytes[5] = fix; put16(bytes.data() + 6, hdop);
  queue(CAN_ID_INFO, bytes); readCan();
}
void output(const char *name) {
  char json[160]; buildJson(json, sizeof(json));
  std::cout << name << '\t' << json << '\n';
}
void logOutput(const char *name) {
  handleCanLog(); std::cout << name << '\t' << server.response << '\n';
}
int main() {
  static_assert(sizeof(canLog) <= 4096, "Log should use bounded RAM");
  static_assert(CAN_LOG_CAPACITY == 64, "Test covers the 64-frame history");
  assert(FIX_TIMEOUT_MS == 5000);
  uint8_t bytes[8]{};
  for (int32_t value : {INT32_MIN, -1800000000, -1, 0, 1, 1800000000, INT32_MAX}) {
    put32(bytes, value); assert(get32(bytes) == value);
  }
  for (uint16_t value : {0, 1, 255, 256, 32768, 65535}) {
    put16(bytes, value); assert(get16(bytes) == value);
  }
  put32(bytes, -338688000);
  assert(bytes[0] == 0x00 && bytes[1] == 0x08 && bytes[2] == 0xD0 && bytes[3] == 0xEB);

  reset(); output("startup"); canReady = false; logOutput("empty_log");
  position(); assert(!seenPosition && canFrames == 0);
  canReady = true; CAN.failRead = true; readCan(); assert(canFrames == 0);

  reset(100); position(); output("position_only");
  reset(100); info(); output("info_only");
  reset(); position(); info(); output("fresh_at_boot");
  clockMs = 4999; output("before_expiry");
  clockMs = 5000; output("at_expiry");

  reset(10); position(); info(); clockMs = 5010; position(); output("info_expired");
  reset(10); position(); info(); clockMs = 5010; info(); output("position_expired");
  reset(100); position(); info(); info(0); output("no_fix_info");
  info(2); output("invalid_fix_flag");

  reset(100); position(); info(1, 0xFFFF, 0); output("optional_unknown");
  info(1, 3600, 0); output("invalid_course");
  info(1, 0, 125); output("north_course");
  clockMs = 200; info(1, 0, 125); clockMs = 350; output("older_position_age");

  reset(100); info();
  position(-900000001, 10); output("latitude_below_range");
  position(900000001, 10); output("latitude_above_range");
  position(10, -1800000001); output("longitude_below_range");
  position(10, 1800000001); output("longitude_above_range");
  position(0, 0); output("zero_coordinate");
  position(-900000000, -1800000000); output("coordinate_lower_bounds");
  position(900000000, 1800000000); output("coordinate_upper_bounds");
  position(0, 730000000); output("equator");
  position(10000000, 0); output("prime_meridian");

  reset(UINT32_MAX - 2000); position(); info(); clockMs = 999; output("wrap_fresh");
  clockMs = 2999; output("wrap_expired");

  reset(100); std::array<uint8_t, 8> payload{};
  put32(payload.data(), 123456789); put32(payload.data() + 4, 987654321);
  queue(CAN_ID_POS, payload, 4);
  queue(0x80000000U | CAN_ID_POS, payload);
  queue(0x40000000U | CAN_ID_POS, payload);
  readCan(); info(); output("malformed_or_flagged_position");
  assert(!seenPosition && canFrames == 4);
  logOutput("flagged_log");
  put32(payload.data(), (int32_t)0xDEADBEEF); queue(CAN_ID_BEAT, payload); readCan();
  assert(rxBeat == 0xDEADBEEF);

  reset(100); position(); info();
  char guarded[20]; memset(guarded, '#', sizeof(guarded));
  buildJson(guarded + 3, 8);
  assert(guarded[2] == '#' && guarded[10] == '\0' && guarded[11] == '#');
  buildJson(guarded + 3, 0); assert(guarded[2] == '#' && guarded[11] == '#');

  reset();
  for (unsigned i = 0; i < 70; i++) {
    for (unsigned j = 0; j < 8; j++) payload[j] = (i + j) & 255;
    clockMs = 123 + i;
    uint32_t id = 0x200 + i;
    if (i % 5 == 0) id |= 0x80000000U;
    if (i % 7 == 0) id |= 0x40000000U;
    queue(id, payload, i % 9); readCan();
  }
  assert(canFrames == 70 && canLogCount == 64 && canLogNext == 6);
  logOutput("wrapped_log");
}
"""


def run():
    host = (ROOT / "esp32_dashboard_host/esp32_dashboard_host.ino").read_text()
    sender = (ROOT / "esp32_gps_sender/esp32_gps_sender.ino").read_text()
    globals_ = host[host.index("bool canReady ="):section(host, "FILE SERVER").start()]
    functions = host[section(host, "CAN RECEIVE").end():section(host, "SPEED API").start()]
    constants = "\n".join(re.findall(r"^#define .*", host, re.MULTILINE))
    # Model the ESP32 ABI without needing desktop 32-bit system libraries.
    extracted = constants + "\n" + globals_ + "\n" + functions + integer_packer(sender)
    extracted = extracted.replace("unsigned long", "uint32_t")
    extracted = extracted.replace("%lu", "%u")
    with tempfile.TemporaryDirectory(prefix="srt-host-can-") as temp:
        cpp, exe = Path(temp) / "test.cpp", Path(temp) / "test"
        cpp.write_text(MOCKS + extracted + CASES)
        subprocess.run(shlex.split(os.environ.get("CXX", "g++")) + [
            "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            str(cpp), "-o", str(exe),
        ], check=True)
        completed = subprocess.run([str(exe)], capture_output=True, text=True)
        if completed.returncode:
            raise RuntimeError(f"C++ tests failed ({completed.returncode}):\n{completed.stderr}")
    results = dict((name, json.loads(value)) for name, value in
                   (line.split("\t", 1) for line in completed.stdout.splitlines()))
    no_fix = {"lat": 0, "lon": 0, "sats": 0, "fix": False}
    fresh = {"lat": -33.8688, "lon": -70.6693, "sats": 9, "speed": 45.6,
             "hdop": 0.87, "ageMs": 0, "course": 123.4, "fix": True}
    expected = {name: no_fix for name in (
        "startup", "position_only", "info_only", "at_expiry", "info_expired",
        "position_expired", "no_fix_info", "invalid_fix_flag", "latitude_below_range",
        "latitude_above_range", "longitude_below_range", "longitude_above_range",
        "zero_coordinate", "wrap_expired", "malformed_or_flagged_position",
    )}
    expected.update({
        "fresh_at_boot": fresh,
        "before_expiry": dict(fresh, ageMs=4999),
        "wrap_fresh": dict(fresh, ageMs=3000),
        "optional_unknown": {key: value for key, value in fresh.items() if key not in ("hdop", "course")},
        "north_course": dict(fresh, hdop=1.25, course=0),
        "older_position_age": dict(fresh, hdop=1.25, course=0, ageMs=250),
        "coordinate_lower_bounds": dict(fresh, lat=-90, lon=-180),
        "coordinate_upper_bounds": dict(fresh, lat=90, lon=180),
        "equator": dict(fresh, lat=0, lon=73),
        "prime_meridian": dict(fresh, lat=1, lon=0),
        "empty_log": {"ready": False, "total": 0, "frames": []},
    })
    expected["invalid_course"] = expected["optional_unknown"]
    packed = lambda lat, lon: list(lat.to_bytes(4, "little", signed=True) + lon.to_bytes(4, "little", signed=True))
    pos_data = packed(123456789, 987654321)
    expected["flagged_log"] = {"ready": True, "total": 4, "frames": [
        {"seq": 1, "ms": 100, "id": 0x100, "dlc": 4, "extended": False, "remote": False, "data": pos_data[:4]},
        {"seq": 2, "ms": 100, "id": 0x100, "dlc": 8, "extended": True, "remote": False, "data": pos_data},
        {"seq": 3, "ms": 100, "id": 0x100, "dlc": 8, "extended": False, "remote": True, "data": []},
        {"seq": 4, "ms": 100, "id": 0x101, "dlc": 8, "extended": False, "remote": False, "data": [200, 1, 210, 4, 9, 1, 87, 0]},
    ]}
    expected["wrapped_log"] = {"ready": True, "total": 70, "frames": [
        {"seq": i + 1, "ms": 123 + i, "id": 0x200 + i, "dlc": i % 9,
         "extended": i % 5 == 0, "remote": i % 7 == 0,
         "data": [] if i % 7 == 0 else [(i + j) & 255 for j in range(i % 9)]}
        for i in range(6, 70)
    ]}
    assert results.keys() == expected.keys(), (results.keys(), expected.keys())
    for name, value in expected.items():
        assert results[name] == value, f"{name}: expected {value!r}, got {results[name]!r}"
    print(f"PASS: {len(expected)} host JSON scenarios, signed CAN roundtrips, heartbeat, "
          "small-buffer guards, and 64-frame ring (ASan/UBSan).")


if __name__ == "__main__":
    run()
