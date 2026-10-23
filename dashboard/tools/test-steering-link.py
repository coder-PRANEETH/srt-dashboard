"""Connect Node 5's real HTTP handler to Node 1's real CAN/GPIO sketch."""
import ast
from pathlib import Path
import re
import subprocess
import tempfile

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
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'node5_dash'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
