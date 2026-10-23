"""Exercise Node 5 joystick commands through actual Node 1 and Node 2 sketches."""
import ast
from pathlib import Path
import re
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[2]
def literal(file,name):
    for n in ast.parse((root/file).read_text()).body:
        if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id==name for t in n.targets):return ast.literal_eval(n.value)
    raise ValueError(name)
before='--before' in sys.argv
host=(root/('build/control-logic-before/node5_dash.ino' if before else 'node5_dash/node5_dash.ino')).read_text()
backend=host[host.index('struct Sample {'):host.index('const char *mime')]
backend+=host[host.index('void sendDiagnostics()'):host.index('void sendDashboard()')]
backend+=host[host.index('bool steeringNodeReady()'):host.index('void setup()')]
shim=literal('dashboard/tools/test-can-protocol.py','shim').replace('struct SerialMock {','struct SerialMock { void begin(int) {}')
common=re.sub(r'^#include .*$', '',literal('dashboard/tools/test-senders.py','common'),flags=re.M)
common=common.replace('uint32_t clockMs=0;','using ::clockMs;').replace('uint32_t millis(){return clockMs;}','using ::millis;')
io=literal('dashboard/tools/test-senders.py','node1_io')
node=re.sub(r'^#include .*$','',(root/'node1_bcm/node1_bcm.ino').read_text(),flags=re.M)
motor_io=literal('dashboard/tools/test-powertrain.py','shim');motor_io=motor_io[motor_io.index('bool canReady=true'):]
motor=re.sub(r'^#include .*$','',(root/'node2_pcm/node2_pcm.ino').read_text(),flags=re.M)
scenarios=r'''
RemoteDriveGate switches;size_t hostAt=0,nodeAt=0,motorAt=0;
uint8_t switchMask=0;
std::deque<std::pair<uint32_t,Input>> delayed;
void receiveHost(const Input &f){incoming.push_back(f);readCan();}
void bus(){
 while(hostAt<outgoing.size()){
  const auto f=outgoing[hostAt++];bcm::Frame n{};n.id=f.id;n.len=f.len;memcpy(n.data,f.data,8);bcm::incoming.push_back(n);
  pcm::servicePowertrain();pcm::receivePowertrain(f.id,f.len,f.data);pcm::servicePowertrain();
  if(f.id==0x120)switches.accept(f.data,f.len,3,f.data[0]==1 || bool(switchMask&SWITCH_REVERSE)==switches.reverse,false,clockMs);
 }
 while(nodeAt<bcm::outgoing.size()){
  const auto f=bcm::outgoing[nodeAt++];Input n{};n.id=f.id;n.len=f.len;memcpy(n.data,f.data,8);
  pcm::receivePowertrain(f.id,f.len,f.data);
  // Variable 0..35 ms CAN/status latency exercises STOP-token ordering.
  delayed.push_back({clockMs+(f.id==0x111?35:0),n});
 }
 for(auto it=delayed.begin();it!=delayed.end();){
  if(int32_t(clockMs-it->first)>=0){receiveHost(it->second);it=delayed.erase(it);}else ++it;
 }
 while(motorAt<pcm::outgoing.size()){
  const auto f=pcm::outgoing[motorAt++];Input n{};n.id=f.id;n.len=f.len;memcpy(n.data,f.data,8);receiveHost(n);
 }
}
void step(){
 bus();bcm::loop();switches.tick(clockMs);
 if(clockMs%100==0){
  Input f{};f.id=0x122;f.len=8;switches.status(f.data,switchMask);receiveHost(f);
  bcm::Frame n{};n.id=f.id;n.len=f.len;memcpy(n.data,f.data,8);bcm::incoming.push_back(n);pcm::receivePowertrain(f.id,f.len,f.data);
  if(switches.phase!=1){uint8_t mask=switches.phase?driveSwitchMask(switchMask,switches.reverse):switchMask;pcm::receivePowertrain(0x300,1,&mask);}
  pcm::sendRemoteMotorStatus();
 }
 pcm::servicePowertrain();if(clockMs%50==0)pcm::sendPowertrainStatus();
 bus();serviceDrive();serviceSteeringRelay();bus();
}
void advance(unsigned ms){const auto until=clockMs+ms;while(clockMs<until)step();}
void move(unsigned seq,unsigned throttle,unsigned steering,unsigned direction,bool reverse=false){
 server.args={{"action","move"},{"token",String(drive.token)},{"seq",String(seq)},{"throttle",String(throttle)},
 {"steering",String(steering)},{"direction",String(direction)},{"reverse",reverse?"1":"0"}};
 handleJoystickCommand();bus();
 if(!drive.held)printf("STOP seq=%u throttle=%u steering=%u direction=%u reason=%s\n",seq,throttle,steering,direction,drive.reason);
 assert(server.code==200 && drive.held);
}
int main(){
 setbuf(stdout,nullptr);pcm::setup();bcm::setup();bcm::inputAdc=930;advance(400);
 server.headers["X-SRT-Control"]="1";assert(joystickCapable());
 for(unsigned sweep=0;sweep<3;++sweep){
  unsigned seq=1;move(seq++,0,0,0);advance(150);
  // Increase to full right at the edge, sweep corners, cross the center axis,
  // and turn back while Node 1's stopped-token acknowledgement is delayed.
  for(unsigned duty=5;duty<=255;duty+=5){move(seq++,180,duty,2);advance(20);}
  for(unsigned k=0;k<8;++k){move(seq++,180,255,k%2?1:2);advance(80);}
  for(unsigned k=0;k<8;++k){move(seq++,255,0,0);advance(60);move(seq++,180,180,k%2?1:2);advance(60);}
  move(seq++,0,255,2);advance(60);move(seq++,0,255,2);advance(60);assert(bcm::pins[33]==255 && bcm::pins[32]==0);
  stopDrive();bus();advance(400);assert(!bcm::pins[32] && !bcm::pins[33] && pcm::pwm[13]==62);
 }
 puts("Actual Node 1 + Node 2 + Node 5 corner/edge sweeps passed: PWM1..255, delayed steering ACKs, reversals, center crossings and idle STOP");
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory)
 if before:(p/'JoystickHost.h').write_text((root/'build/control-logic-before/JoystickHost.h').read_text())
 cpp=p/'vehicle.cpp';cpp.write_text(shim+'\nvoid delay(unsigned n){clockMs+=n;}\nnamespace bcm {\n'+common+io+node+'\n}\nnamespace pcm {\nconstexpr int LOW=0,HIGH=1,OUTPUT=1;\n'+motor_io+motor+'\n}\n'+backend+scenarios)
 binary=p/'vehicle';subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(root/'node5_dash'),str(cpp),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
