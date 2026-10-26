"""Connect Node 5's real HTTP handler to Node 1's real CAN/GPIO sketch."""
import ast
from pathlib import Path
import re
import subprocess
import tempfile
from gps_test_support import gps_build_flags

root = Path(__file__).resolve().parents[2]


def literal(file, name):
    for node in ast.parse((root / file).read_text()).body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == name for t in node.targets):
            return ast.literal_eval(node.value)
    raise ValueError(name)


host = (root / 'node5_dash/node5_dash.ino').read_text()
backend = host[host.index('struct Sample {'):host.index('const char *mime')]
backend += host[host.index('void sendDiagnostics()'):host.index('void sendDashboard()')]
backend += host[host.index('bool steeringNodeReady()'):host.index('void setup()')]
shim = literal('dashboard/tools/test-can-protocol.py', 'shim')
common = literal('dashboard/tools/test-senders.py', 'common')
common = re.sub(r'^#include .*$', '', common, flags=re.M)
common = common.replace('uint32_t clockMs=0;', 'using ::clockMs;').replace('uint32_t millis(){return clockMs;}', 'using ::millis;')
io = literal('dashboard/tools/test-senders.py', 'node1_io')
node = re.sub(r'^#include .*$', '', (root / 'node1_bcm/node1_bcm.ino').read_text(), flags=re.M)
scenarios = r'''
size_t hostDelivered=0,nodeDelivered=0;
void deliverHost() {
  while(hostDelivered<outgoing.size()) {
    const auto f=outgoing[hostDelivered++];
    bcm::Frame frame{};frame.id=f.id;frame.len=f.len;memcpy(frame.data,f.data,8);
    bcm::incoming.push_back(frame);
  }
}
void deliverNode() {
  push(0x300,{SWITCH_REMOTE_MODE});
  while(nodeDelivered<bcm::outgoing.size()) {
    const auto f=bcm::outgoing[nodeDelivered++];
    Input frame{};frame.id=f.id;frame.len=f.len;memcpy(frame.data,f.data,8);
    incoming.push_back(frame);
  }
  readCan();
}
void advance(unsigned ms) {
  const auto until=clockMs+ms;
  while(clockMs<until) {
    deliverHost();bcm::loop();deliverNode();serviceSteeringRelay();
  }
}
void command(const char *direction,unsigned seq=1) {
  server.args={{"direction",direction},{"token",String(steeringRelay.token)},{"seq",String(seq)}};
  handleSteeringCommand();
}
int main() {
  setbuf(stdout,nullptr);bcm::setup();bcm::inputAdc=795;
  server.headers["X-SRT-Control"]="1";server.headers["Origin"]="http://192.168.4.1";
  push(0x300,{SWITCH_REMOTE_MODE});readCan();
  sendSteeringState();assert(!steeringNodeReady());
  assert(server.body.find("WAITING FOR NODE 1 CAN STATUS")!=std::string::npos);
  advance(150);sendSteeringState();assert(steeringNodeReady());
  assert(server.body.find("\"angleAvailable\":false")!=std::string::npos);
  assert(server.body.find("\"controlVersion\":3")!=std::string::npos);
  assert(server.body.find("\"angleRequired\":false")!=std::string::npos);
  for(const auto direction : {"left","right"}) {
    command(direction);assert(server.code==200);
    const auto frame=outgoing.back();
    assert(frame.id==0x110 && frame.len==8 && frame.data[0]==(direction[0]=='l'?1:2) && frame.data[1]==60);
    assert(steeringRead32(frame.data+4)==bcm::steering.token);
    advance(20);
    assert(bcm::pins[direction[0]=='l'?32:33]==60 && bcm::pins[direction[0]=='l'?33:32]==0);
    for(unsigned seq=2;seq<=10;++seq) {advance(80);command(direction,seq);assert(server.code==200);}
    advance(20);sendSteeringState();assert(bcm::remoteSteeringAccepted>=10);
    assert(server.body.find("\"pwm\":60")!=std::string::npos);
    command("stop");advance(20);assert(!bcm::pins[32] && !bcm::pins[33]);
  }
  auto hornCommand=[](const char *action,unsigned seq=1){
    server.args={{"action",action},{"token",String(hornRelay.token)},{"seq",String(seq)}};handleHornCommand();
  };
  advance(150);sendHornState();assert(hornNodeReady());
  hornCommand("press");assert(server.code==200);advance(20);assert(bcm::pins[13]==bcm::HORN_RELAY_ON);
  sendVehicle();assert(server.body.find("\"horn\":true")!=std::string::npos);
  assert(server.body.find("\"hornWebsitePressed\":true")!=std::string::npos);
  for(unsigned seq=2;seq<90;++seq){advance(80);hornCommand("press",seq);assert(server.code==200);assert(bcm::hornRelayOn);}
  assert(bcm::websiteHorn.held); // More than seven seconds, with no total cap.
  IMUDataWithHorn wireless={20,1,1};
  bcm::onSteeringAngle(nullptr,(const uint8_t*)&wireless,sizeof(wireless));advance(20);
  hornCommand("stop");advance(20);assert(!bcm::websiteHorn.held && bcm::hornRelayOn);
  ++wireless.sequence;wireless.hornPressed=0;bcm::onSteeringAngle(nullptr,(const uint8_t*)&wireless,sizeof(wireless));advance(20);
  assert(!bcm::hornRelayOn && bcm::pins[13]==bcm::HORN_RELAY_OFF);
  hornCommand("press");assert(server.code==200);advance(20);
  ++wireless.sequence;wireless.hornPressed=1;bcm::onSteeringAngle(nullptr,(const uint8_t*)&wireless,sizeof(wireless));advance(20);
  ++wireless.sequence;wireless.hornPressed=0;bcm::onSteeringAngle(nullptr,(const uint8_t*)&wireless,sizeof(wireless));advance(20);
  assert(bcm::hornRelayOn); // Wireless release cannot cancel website.
  const auto expired=bcm::websiteHorn.token;
  advance(351);assert(!bcm::websiteHorn.held && !bcm::hornRelayOn && bcm::websiteHorn.token!=expired);
  // A delayed press with an expired token cannot re-energize the relay.
  bcm::Frame replay{};replay.id=HORN_COMMAND_ID;replay.len=8;replay.data[0]=replay.data[1]=1;replay.data[2]=9;
  steeringWrite32(replay.data+4,expired);bcm::incoming.push_back(replay);advance(20);assert(!bcm::hornRelayOn);
  // Node1 watchdog alone works if dashboard stops running altogether.
  hornCommand("press");assert(server.code==200);deliverHost();bcm::loop();assert(bcm::hornRelayOn);
  clockMs+=350;bcm::loop();assert(!bcm::hornRelayOn);deliverNode();serviceSteeringRelay();
  hornCommand("press");assert(server.code==503);advance(150); // Wait for pending STOP acknowledgement.
  hornCommand("press");assert(server.code==200);advance(20);assert(bcm::hornRelayOn);
  push(0x300,{0});readCan();serviceHornHost();deliverHost();bcm::loop();assert(!bcm::hornRelayOn);
  // Physical remote switch gates website horn but leaves wireless horn usable.
  push(0x300,{0});readCan();hornCommand("press");assert(server.code==503);deliverHost();bcm::loop();
  ++wireless.sequence;wireless.hornPressed=1;bcm::onSteeringAngle(nullptr,(const uint8_t*)&wireless,sizeof(wireless));bcm::loop();assert(bcm::hornRelayOn);
  bcm::canTxWorks=false;bcm::loop();assert(bcm::hornRelayOn); // CAN TX failure does not mute wireless.
  bcm::canTxWorks=true;
  clockMs+=250;bcm::loop();assert(!bcm::hornRelayOn);deliverNode();advance(150);
  server.headers["Origin"]="http://other-host";hornCommand("press");assert(server.code==403);
  server.headers["Origin"]="http://192.168.4.1";
  puts("Horn actual HTTP/CAN/GPIO integration passed: >7s hold, independent sources, wireless/website release, leases, stale tokens, remote mode gate and origin validation");
  command("left");advance(20);assert(bcm::pins[32]==60);
  advance(351);assert(!bcm::pins[32] && !bcm::pins[33]);
  // A missing status return is distinguishable from dashboard HTTP failure.
  clockMs+=600;sendSteeringState();assert(!steeringNodeReady());
  assert(server.body.find("NODE 1 CAN STATUS STALE")!=std::string::npos);
  puts("Actual HTTP -> 0x110 -> Node 1 GPIO32/33 -> 0x111 return path passed: left/right at PWM60, renewals, STOP, expiry, no IMU and link diagnostics");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    cpp = path / 'steering-link.cpp'
    cpp.write_text(shim + '\nnamespace bcm {\n' + common + io + node + '\n}\n' + backend + scenarios)
    binary = path / 'steering-link'
    subprocess.run(['g++', *gps_build_flags(Path(directory)), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'node5_dash'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
