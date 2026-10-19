"""Exercise Node 5's actual CAN receiver/API code with a native mocked bus.

Run: python3 dashboard/tools/test-can-protocol.py (requires a C++17 compiler).
The sketch's decoder/API functions are compiled unchanged; only Arduino I/O
and String are replaced. No ESP32 or physical CAN controller is needed.
"""
from pathlib import Path
import json
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'node5_dash.ino').read_text()
backend = source[source.index('struct Sample {'):source.index('const char *mime')]
backend += source[source.index('void sendDiagnostics()'):source.index('void sendDashboard()')]
backend += source[source.index('bool steeringNodeReady()'):source.index('void setup()')]
shim = r'''
#include <algorithm>
#include "ThrottleCalibration.h"
#include "SteeringControl.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <vector>
#include <cassert>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
using std::isfinite;
class String : public std::string {
 public:
  using std::string::string;
  String(const std::string &s) : std::string(s) {}
  template<class T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
  String(T n) : std::string(std::to_string(n)) {}
  String(double n, unsigned decimals) {
    std::ostringstream out; out << std::fixed << std::setprecision(decimals) << n;
    assign(out.str());
  }
  bool startsWith(const char *p) const { return rfind(p, 0) == 0; }
  void trim() {
    auto a = find_first_not_of(" \t\r\n"), b = find_last_not_of(" \t\r\n");
    *this = a == npos ? "" : substr(a, b - a + 1);
  }
};
uint32_t clockMs = 1000;
uint32_t millis() { return clockMs; }
bool canReady = true;
uint8_t canTxCount = 0, canRxCount = 0;
uint32_t canTxTotal = 3, canRxTotal = 0, canTxFailed = 2, canLastRxAt = 0;
struct EspMock { unsigned getFreeHeap(){return 140000;} unsigned getMinFreeHeap(){return 130000;} unsigned getMaxAllocHeap(){return 110000;} } ESP;
struct FsMock { unsigned totalBytes(){return 1900000;} unsigned usedBytes(){return 1500000;} } LittleFS;
struct WifiMock { unsigned softAPgetStationNum(){return 1;} } WiFi;
struct SerialMock { template<class... T> void printf(const char*, T...) {} } Serial;
struct Server {
  String filter, body; int code = 0;
  std::map<std::string, String> args, headers;
  String arg(const char *key) { return std::string(key)=="id" ? filter : args[key]; }
  String header(const char *key) { return headers[key]; }
  bool hasHeader(const char *key) { return headers.count(key); }
  String hostHeader() { return "192.168.4.1"; }
  void sendHeader(const char*, const char*) {}
  void send(int status, const char*, const String &text) { code = status; body = text; }
} server;
struct Input { uint32_t id; uint8_t len, data[8] = {}; };
std::vector<Input> outgoing;
bool txWorks = true;
template <size_t N> bool canSend(uint32_t id, const uint8_t (&bytes)[N]) {
  Input f{}; f.id=id; f.len=N; std::copy(bytes,bytes+N,f.data); outgoing.push_back(f); return txWorks;
}
std::deque<Input> incoming;
void push(uint32_t id, std::initializer_list<uint8_t> bytes) {
  Input f{}; f.id = id; f.len = bytes.size(); std::copy(bytes.begin(), bytes.end(), f.data);
  incoming.push_back(f);
}
bool canRead(uint32_t &id, uint8_t &len, uint8_t (&data)[8]) {
  if (incoming.empty()) return false;
  auto f = incoming.front(); incoming.pop_front();
  id = f.id; len = f.len; memcpy(data, f.data, 8); ++canRxCount; ++canRxTotal; canLastRxAt = millis(); return true;
}
'''
scenarios = r'''
void record(const char *label) { std::cout << label << '\t' << server.code << '\t' << server.body << '\n'; }
void vehicle(const char *label) { sendVehicle(); record(label); }
void log(const char *label, const char *filter) { server.filter = filter; sendCanLog(); record(label); }
int main() {
  vehicle("empty"); sendDiagnostics(); record("diagnostics-empty");
  push(0x301, {0xD0,0x07}); // 2000, deliberately different from big-endian decoding
  push(0x300, {0x25}); // cruise + reverse + headlight, autonomous OFF
  vehicle("live"); log("short-log", ""); log("filter", "0x301");
  clockMs = 1500; vehicle("at-500ms");
  clockMs = 1501; vehicle("stale-pedal");
  // Old payloads and out-of-range ADC must not refresh valid data.
  push(0x301, {7,208,48,0,0,0,0,0}); push(0x300, {1,0,1,0,0,1,0,0});
  push(0x301, {0,16}); push(0x301, {}); push(0x300, {});
  vehicle("malformed"); sendDiagnostics(); record("diagnostics-malformed");
  clockMs = 4000; vehicle("at-3000ms");
  clockMs = 4001; vehicle("stale-switches");
  push(0x301, {0,0}); vehicle("zero");
  push(0x301, {0x64,0x03}); vehicle("below-idle");
  push(0x301, {255,15}); vehicle("full");
  for (int bit = 0; bit < 7; ++bit) {
    push(0x300, {uint8_t(1 << bit)});
    const std::string label = "bit-" + std::to_string(bit); vehicle(label.c_str());
  }
  push(0x400, {10,17,16,79,0,117,1,8});
  push(0x401, {1,0,120,0,0,0,0,0});
  push(0x500, {1,85,0,1,0,0,0,0});
  push(0x600, {4,1,4,0,10,20,0,0});
  vehicle("node4"); sendGps(); record("gps");
  // Invalid coordinates and IMU encoding must not replace the last good sample.
  push(0x400, {90,1,0,79,0,0,1,8}); push(0x500, {1,181,0,1,0,0,0,0});
  vehicle("invalid-domain"); sendDiagnostics(); record("diagnostics-domain");
  log("no-match", "1FFFFFFF"); log("invalid-filter", "301junk");
  for (int n = 0; n < 70; ++n) push(0x301, {uint8_t(n),0});
  log("ring", "301");
  // Unsigned millis subtraction must also work over wraparound.
  clockMs = 0xFFFFFFF0; push(0x301, {1,0}); push(0x300, {0}); vehicle("pre-wrap");
  clockMs = 100; vehicle("post-wrap");
  fsReady = true; push(0x777, {1}); sendDiagnostics(); record("diagnostics-wrap");
  // Exercise the actual HTTP handler with a confirmed Node 1 capability frame.
  handleSteeringCommand(); assert(server.code==403 && outgoing.empty());
  server.headers["X-SRT-Control"]="1"; server.headers["Origin"]="http://other-host";
  handleSteeringCommand(); assert(server.code==403 && outgoing.empty());
  server.headers["Origin"]="http://192.168.4.1";
  server.args["direction"]="left"; server.args["token"]=String(steeringRelay.token); server.args["seq"]="1";
  handleSteeringCommand(); assert(server.code==503 && outgoing.back().data[0]==0);
  push(0x111, {0,0,0,1,9,0,0,0}); readCan();
  server.args["token"]=String(steeringRelay.token);
  handleSteeringCommand(); assert(server.code==200 && outgoing.back().id==0x110);
  assert(outgoing.back().data[0]==1 && outgoing.back().data[1]==60 && outgoing.back().data[4]==9);
  handleSteeringCommand(); assert(server.code==409 && outgoing.back().data[0]==0); // replay
  server.args["token"]=String(steeringRelay.token); server.args["seq"]="2junk";
  handleSteeringCommand(); assert(server.code==400 && outgoing.back().data[0]==0);
  server.args["seq"]="1"; server.args["token"]=String(steeringRelay.token);
  handleSteeringCommand(); assert(server.code==200);
  clockMs+=350; serviceSteeringRelay(); assert(outgoing.back().data[0]==0);
  server.args["seq"]="2"; handleSteeringCommand(); assert(server.code==409); // expired gesture
  server.args["token"]=String(steeringRelay.token); server.args["seq"]="1"; txWorks=false;
  handleSteeringCommand(); assert(server.code==503 && !steeringRelay.direction); txWorks=true;
  clockMs+=600; server.args["token"]=String(steeringRelay.token);
  handleSteeringCommand(); assert(server.code==503 && outgoing.back().data[0]==0);
  server.args["direction"]="stop"; handleSteeringCommand(); assert(server.code==200 && outgoing.back().data[0]==0);

}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'check.cpp').write_text(shim + backend + scenarios)
    subprocess.run(['g++', '-I', str(root), '-std=c++17', '-Wall', '-Wextra', str(path / 'check.cpp'), '-o', str(path / 'check')], check=True)
    lines = subprocess.check_output([str(path / 'check')], text=True).splitlines()
results = {}
for line in lines:
    name, status, body = line.split('\t', 2)
    results[name] = (int(status), json.loads(body))
def result(name):
    status, body = results[name]
    assert status == 200, (name, status)
    return body

assert result('empty')['acceleratorAdc'] is None
assert result('empty')['nodes'][1]['alive'] is None
live = result('live')
assert live['acceleratorAdc'] == 2000 and live['acceleratorPct'] == 35
assert live['acceleratorVoltageEstimate'] == 0.912
assert live['acceleratorRawVoltageEstimate'] == 1.612
assert live['throttleOut'] is None and live['reverseActive'] is None
assert live['switches']['headlight'] is True and live['switches']['autonomous'] is False
assert live['nodes'][0]['alive'] is True and live['nodes'][2]['alive'] is True
assert live['nodes'][1]['alive'] is None and live['nodes'][1]['source'] == 'receive-only'
frames = result('short-log')['frames']
assert [frame['dlc'] for frame in frames] == [2, 1]
assert [frame['data'] for frame in frames] == [[0xD0, 7], [0x25]]
assert len(result('filter')['frames']) == 1 and result('filter')['frames'][0]['id'] == 0x301
assert result('at-500ms')['acceleratorAdc'] == 2000
for name in ['stale-pedal', 'malformed']:
    assert result(name)['acceleratorAdc'] is None
    assert result(name)['switches']['headlight'] is True
assert result('malformed')['nodes'][0]['ageMs'] == 501
assert result('at-3000ms')['switches']['headlight'] is True
assert result('stale-switches')['switches']['headlight'] is None
assert result('zero')['acceleratorPct'] == 0
assert result('full')['acceleratorAdc'] == 4095 and result('full')['acceleratorPct'] == 100
names = ['cruise', 'eco', 'reverse', 'hill', 'horn', 'headlight', 'autonomous']
for bit, name in enumerate(names):
    assert result(f'bit-{bit}')['switches'] == {key: key == name for key in names}
assert result('node4')['pitchDeg'] == -5 and result('node4')['ignitionUptime'] == 120
assert result('node4')['nodes'][3]['alive'] is True
assert result('gps')['fix'] is True and result('gps')['sats'] == 8
assert result('no-match')['frames'] == []
assert results['invalid-filter'][0] == 400
assert len(result('ring')['frames']) == 64
assert result('ring')['frames'][0]['data'] == [6, 0]
assert result('ring')['frames'][-1]['data'] == [69, 0]
assert result('post-wrap')['nodes'][0]['alive'] is True
assert result('post-wrap')['nodes'][2]['alive'] is True
print('CAN receiver/API tests passed: payloads, endian, all switch bits, freshness, malformed frames, GPS, filter, ring buffer, clock wrap')

d = result('diagnostics-empty')
assert d['freeHeap'] == 140000 and d['minFreeHeap'] == 130000
assert d['lastRxAgeMs'] is None and d['fsTotal'] == 0 and not d['fsReady']
assert all(not x['seen'] and x['ageMs'] is None for x in d['signals'])
d = result('diagnostics-malformed')
assert d['malformedFrames'] == 5 and d['signals'][0]['invalid'] == 3
assert d['signals'][0]['ageMs'] == 501 and not d['signals'][0]['fresh']
d = result('diagnostics-wrap')
assert d['signals'][0]['ageMs'] == 116 and d['signals'][0]['fresh']
assert d['unknownFrames'] == 1 and d['lastRxAgeMs'] == 0
assert d['fsReady'] and d['fsUsed'] == 1500000 and d['txFailed'] == 2
print('Diagnostics tests passed: counters, missing sensors, memory, storage, clock wrap')

assert result('invalid-domain')['pitchDeg'] == -5
assert result('diagnostics-domain')['malformedFrames'] == 7
assert result('diagnostics-domain')['signals'][2]['invalid'] == 1
assert result('diagnostics-domain')['signals'][4]['invalid'] == 1
print('Invalid GPS and IMU payloads cannot replace valid samples')

assert result("below-idle")["acceleratorVoltageEstimate"] == 0
assert result("below-idle")["acceleratorPct"] == 0
assert result("full")["acceleratorVoltageEstimate"] == 2.6

print("Steering HTTP tests passed: origin/header checks, node capability, PWM60, replay, invalid input, expiry and CAN failure")
