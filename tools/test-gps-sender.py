#!/usr/bin/env python3
"""Run the sender sketch with real TinyGPSPlus and desktop hardware stubs.

Requires g++ and the TinyGPSPlus Arduino library. This checks GPS/CAN logic,
not UART, SPI, CAN electrical behaviour or the ESP32 runtime.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile


ARDUINO = r'''
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
using std::min;
using std::max;
using std::isfinite;
using byte = uint8_t;
constexpr double PI = 3.14159265358979323846;
constexpr double TWO_PI = 2.0 * PI;
inline double radians(double x) { return x * PI / 180.0; }
inline double degrees(double x) { return x * 180.0 / PI; }
inline double sq(double x) { return x * x; }
unsigned long millis();
void delay(unsigned long ms);
inline void pinMode(int, int) {}
constexpr int INPUT = 0;
constexpr int SERIAL_8N1 = 0;
class String {
  std::string data;
public:
  explicit String(uint16_t value) : data(std::to_string(value)) {}
  const char *c_str() const { return data.c_str(); }
};
class SerialStub {
public:
  void begin(int) {}
  int available() { return 0; }
  int read() { return -1; }
  void write(char) {}
  template<class... Args> void print(Args...) {}
  template<class... Args> void println(Args...) {}
  template<class... Args> void printf(Args...) {}
};
extern SerialStub Serial;
class HardwareSerial : public SerialStub {
public:
  explicit HardwareSerial(int) {}
  void setRxBufferSize(int) {}
  void begin(int, int, int, int) {}
};
'''

CAN = r'''
#pragma once
#include <array>
#include <cstdint>
#include <vector>
using INT8U = uint8_t;
using INT32U = uint32_t;
constexpr int CAN_500KBPS = 0;
constexpr int MCP_8MHZ = 0;
constexpr int MCP_ANY = 0;
constexpr int MCP_NORMAL = 0;
constexpr int CAN_OK = 0;
struct CanFrame { INT32U id; std::array<INT8U, 8> data; };
extern std::vector<CanFrame> frames;
class MCP_CAN {
public:
  explicit MCP_CAN(int) {}
  int begin(int, int, int) { return CAN_OK; }
  int setMode(int) { return CAN_OK; }
  int sendMsgBuf(INT32U id, int, int len, INT8U *data) {
    if (len != 8) return 1;
    CanFrame frame{id, {}};
    std::copy(data, data + len, frame.data.begin());
    frames.push_back(frame);
    return CAN_OK;
  }
};
'''

TESTS = r'''
#include "Arduino.h"
#include "mcp_can.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>
unsigned long testNow = 0;
unsigned long millis() { return testNow; }
void delay(unsigned long ms) { testNow += ms; }
SerialStub Serial;
std::vector<CanFrame> frames;
#include "SKETCH_PATH"

void reset() {
  testNow = 1000;
  gps = TinyGPSPlus();
  outValid = false;
  outLat = outLon = lastRawLat = lastRawLon = 0;
  heldCount = rejectedJumpCount = lastAcceptedAt = 0;
  sentOk = sentFail = beat = 0;
  frames.clear();
}

void sentence(const std::string &body) {
  unsigned char checksum = 0;
  for (char c : body) checksum ^= c;
  char suffix[8];
  snprintf(suffix, sizeof(suffix), "*%02X\r\n", checksum);
  for (char c : "$" + body + suffix) gps.encode(c);
}

void gga(unsigned long now, const std::string &lat = "1043.6800",
         const std::string &hdop = "1.0", const std::string &quality = "1") {
  testNow = now;
  sentence("GPGGA,123519," + lat + ",N,07901.1700,E," + quality +
           ",08," + hdop + ",0.0,M,0.0,M,,");
}

void rmc(unsigned long now, const std::string &lat = "1043.6800",
         const std::string &speed = "10.0", const std::string &ns = "N",
         const std::string &ew = "E") {
  testNow = now;
  sentence("GPRMC,123519,A," + lat + "," + ns + ",07901.1700," + ew + "," + speed +
           ",90.0,101026,,,A");
}

void fix(unsigned long now, const std::string &lat = "1043.6800",
         const std::string &speed = "10.0", const std::string &hdop = "1.0") {
  rmc(now, lat, speed);
  gga(now, lat, hdop);
}

void send() {
  frames.clear();
  sendCan();
  assert(frames.size() == 3);
  assert(frames[0].id == CAN_ID_POS);
  assert(frames[1].id == CAN_ID_INFO);
  assert(frames[2].id == CAN_ID_BEAT);
}

uint16_t u16(const std::array<INT8U, 8> &b, int offset) {
  return b[offset] | (uint16_t(b[offset + 1]) << 8);
}
int32_t i32(const std::array<INT8U, 8> &b, int offset) {
  return int32_t(uint32_t(b[offset]) | (uint32_t(b[offset + 1]) << 8) |
                 (uint32_t(b[offset + 2]) << 16) |
                 (uint32_t(b[offset + 3]) << 24));
}

int main() {
  reset();
  send();
  assert(frames[1].data[5] == 0);

  fix(1000);
  send();
  assert(frames[1].data[5] == 1);
  assert(std::abs(i32(frames[0].data, 0) - 107280000) <= 1);
  assert(std::abs(i32(frames[0].data, 4) - 790195000) <= 1);
  assert(u16(frames[1].data, 0) == 185);
  assert(u16(frames[1].data, 2) == 900);
  assert(frames[1].data[4] == 8);
  assert(u16(frames[1].data, 6) == 100);
  std::cout << "PASS initial fix and three-frame binary CAN layout\n";

  fix(2000, "1043.6900");
  assert(gps.location.isUpdated());
  printStatus();
  assert(gps.location.isUpdated());
  const double before = outLat;
  send();
  assert(outLat > before);
  std::cout << "PASS diagnostics preserve the next location update\n";

  testNow = 7000;
  send();
  assert(frames[1].data[5] == 0);
  assert(i32(frames[0].data, 0) == 0);
  assert(u16(frames[1].data, 2) == 0xFFFF);
  std::cout << "PASS disconnected GPS expires while CAN stays active\n";

  reset();
  fix(1000);
  send();
  for (unsigned long t = 2000; t <= 6000; t += 1000) {
    fix(t, "1043.6800", "10.0", "9.0");
    send();
  }
  assert(frames[1].data[5] == 0);
  assert(!filteredFix());
  fix(7000);
  send();
  assert(frames[1].data[5] == 1);
  std::cout << "PASS poor HDOP expires the held fix and good HDOP reacquires\n";

  reset();
  fix(1000);
  send();
  for (unsigned long t = 2000; t <= 6000; t += 1000) {
    fix(t, "1044.6800");
    send();
  }
  assert(frames[1].data[5] == 0);
  assert(rejectedJumpCount == 5);
  fix(7000);
  send();
  assert(frames[1].data[5] == 1);
  std::cout << "PASS repeated rejected jumps expire the fix without bypassing the filter\n";

  reset();
  fix(1000);
  send();
  for (unsigned long t = 2000; t <= 6000; t += 1000) {
    gga(t);
    send();
  }
  assert(frames[1].data[5] == 1);
  assert(u16(frames[1].data, 0) == 0);
  assert(u16(frames[1].data, 2) == 0xFFFF);
  std::cout << "PASS current GGA cannot keep stale RMC speed/course alive\n";

  reset();
  fix(1000);
  send();
  for (unsigned long t = 2000; t <= 6000; t += 1000) {
    rmc(t);
    send();
  }
  assert(frames[1].data[5] == 1);
  assert(frames[1].data[4] == 0);
  assert(u16(frames[1].data, 6) == 0);
  std::cout << "PASS current RMC cannot keep stale GGA HDOP/satellites alive\n";

  reset();
  rmc(1000, "1043.6800", "10.0", "S", "W");
  send();
  assert(frames[1].data[5] == 1);
  assert(std::abs(i32(frames[0].data, 0) + 107280000) <= 1);
  assert(std::abs(i32(frames[0].data, 4) + 790195000) <= 1);
  std::cout << "PASS southern/western coordinates retain their sign on CAN\n";

  reset();
  fix(1000, "1043.6800", "0.0");
  send();
  const double parked = outLat;
  fix(2000, "1043.6810", "0.0");
  send();
  assert(outLat == parked);
  fix(3000, "1043.8000", "0.0");
  send();
  assert(outLat == parked);
  assert(rejectedJumpCount == 1);
  std::cout << "PASS stationary deadband and implausible jump rejection\n";

  reset();
  fix(1000, "9943.6800");
  send();
  assert(frames[1].data[5] == 0);
  std::cout << "PASS malformed out-of-range coordinates cannot become a fix\n";

  reset();
  fix(1000);
  send();
  for (unsigned long t = 2000; t <= 6000; t += 1000) {
    gga(t, "1043.6800", "1.0", "0");
    send();
  }
  assert(frames[1].data[5] == 0);
  std::cout << "PASS ongoing no-fix NMEA does not refresh an old position\n";
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path,
                        default=Path.home() / 'Arduino/libraries/TinyGPSPlus',
                        help='installed TinyGPSPlus library directory')
    args = parser.parse_args()
    library = args.library.resolve() / 'src'
    if not (library / 'TinyGPS++.cpp').is_file():
        parser.error(f'TinyGPSPlus not found at {library}; use --library')

    repo = Path(__file__).resolve().parent.parent
    sketch = repo / 'esp32_gps_sender/esp32_gps_sender.ino'
    with tempfile.TemporaryDirectory(prefix='gps-sender-test-') as directory:
        work = Path(directory)
        (work / 'Arduino.h').write_text(ARDUINO)
        (work / 'SPI.h').write_text('#pragma once\n')
        (work / 'mcp_can.h').write_text(CAN)
        (work / 'test.cpp').write_text(TESTS.replace('SKETCH_PATH', str(sketch)))
        executable = work / 'test-sender'
        subprocess.run([
            'g++', '-std=c++17', '-DARDUINO=10819', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-Wno-implicit-fallthrough',
            '-I', str(work), '-I', str(library),
            str(work / 'test.cpp'), str(library / 'TinyGPS++.cpp'),
            '-o', str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
