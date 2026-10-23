"""Run Node 5 control handlers against the actual Node 2 powertrain code."""
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
shim = shim.replace('struct SerialMock {', 'struct SerialMock { void begin(int) {}')
motor_io = literal('dashboard/tools/test-powertrain.py', 'shim')
motor_io = motor_io[motor_io.index('bool canReady=true'):]
motor = re.sub(r'^#include .*$', '', (root / 'node2_pcm/node2_pcm.ino').read_text(), flags=re.M)
scenarios = r'''
RemoteDriveGate pedalGate, switchGate;
SteeringControl steeringNode;
uint16_t physical=930; // ~0.75 V must permit entry.
uint8_t physicalSwitches=0;
size_t hostDelivered=0,motorDelivered=0;
void bus() {
  while(hostDelivered<outgoing.size()) {
    const auto f=outgoing[hostDelivered++];
    if(f.id==DRIVE_GATE_ID) {
      pedalGate.accept(f.data,f.len,1,physical<=DRIVE_IDLE_ADC,
        switchGate.phase==0 && switchGate.session==pedalGate.session,clockMs);
      switchGate.accept(f.data,f.len,3,f.data[0]==1 || bool(physicalSwitches&SWITCH_REVERSE)==switchGate.reverse,false,clockMs);
    }
    if(f.id==STEERING_COMMAND_ID) steeringNode.accept(f.data,f.len,clockMs);
    pcm::servicePowertrain();pcm::receivePowertrain(f.id,f.len,f.data);pcm::servicePowertrain();
  }
  while(motorDelivered<pcm::outgoing.size()) {
    const auto f=pcm::outgoing[motorDelivered++];
    Input frame{};frame.id=f.id;frame.len=f.len;memcpy(frame.data,f.data,8);incoming.push_back(frame);
  }
  readCan();
}
void step() {
  ++clockMs;
  pedalGate.tick(clockMs);switchGate.tick(clockMs);steeringNode.tick(clockMs);
  if(clockMs%100==0) {
    for(int which=0;which<2;++which) {
      Input f{};f.id=which?0x122:0x121;f.len=8;
      if(which) switchGate.status(f.data,physicalSwitches); else pedalGate.status(f.data,physical);
      incoming.push_back(f);pcm::receivePowertrain(f.id,f.len,f.data);
    }
    Input st{};st.id=0x111;st.len=8;steeringNode.status(st.data);incoming.push_back(st);
    pcm::sendRemoteMotorStatus();
  }
  if(clockMs%50==0 && pedalGate.phase!=1) {
    uint16_t adc=pedalGate.phase?DRIVE_REST_ADC:physical;
    uint8_t d[2]={(uint8_t)adc,(uint8_t)(adc>>8)};pcm::receivePowertrain(0x301,2,d);
  }
  if(clockMs%100==0 && switchGate.phase!=1) {
    uint8_t mask=switchGate.phase?driveSwitchMask(physicalSwitches,switchGate.reverse):physicalSwitches;
    pcm::receivePowertrain(0x300,1,&mask);
  }
  pcm::servicePowertrain();
  if(pedalGate.phase && pcm::stopReason==0) assert(pcm::pwm[13]>=62 && pcm::pwm[14]>=62);
  if(clockMs%50==0) pcm::sendPowertrainStatus();
  bus();serviceDrive();serviceSteeringRelay();bus();
}
void advance(unsigned ms) {for(unsigned n=0;n<ms;++n)step();}
void button(unsigned seq,unsigned percent=25) {
  server.args={{"action","throttle"},{"token",String(drive.token)},{"seq",String(seq)},{"percent",String(percent)}};
  handleDriveCommand();bus();
}
void joystick(unsigned seq,unsigned pwm=100,unsigned dir=0,unsigned steer=0,bool reverse=false) {
  server.args={{"action","move"},{"token",String(drive.token)},{"seq",String(seq)},{"throttle",String(pwm)},
    {"direction",String(dir)},{"steering",String(steer)},{"reverse",reverse?"1":"0"}};
  handleJoystickCommand();bus();
}
void reverseButton(unsigned seq,bool reverse,uint32_t token) {
  server.args={{"action","reverse"},{"token",String(token)},{"seq",String(seq)},{"reverse",reverse?"1":"0"}};
  handleDriveCommand();bus();
}
int main() {
  setbuf(stdout,nullptr);pcm::setup();clockMs=10000;advance(1000);
  server.headers["X-SRT-Control"]="1";
  physical=993;advance(100);button(1);assert(server.code==409 && !drive.held);
  physical=992;advance(100);
  // Compare the entire output curve, using each browser's real renewal
  // cadence (button 80 ms, joystick 50 ms), not just the final target value.
  auto fullTrace=[&](bool buttons) {
    stopDrive();bus();advance(400);advance((100-clockMs%100)%100);
    std::vector<int> curve;unsigned sequence=0;int previous=62;
    for(unsigned elapsed=0;elapsed<1600;++elapsed) {
      if(elapsed==400)physical=4095; // Pedal must be ignored during remote ownership.
      if(elapsed%(buttons?80:50)==0) {
        if(buttons)button(++sequence,100);else joystick(++sequence,255);
        assert(server.code==200 && drive.held);
      }
      step();const int output=pcm::pwm[13];curve.push_back(output);
      assert(output>=previous && output-previous<=1 && pcm::pwm[14]==output);previous=output;
    }
    assert(curve.front()==62 && curve.back()==255 && drive.held);
    stopDrive();bus();assert(pcm::pwm[13]==62);advance(400);
    assert(pcm::pwm[13]==62 && pedalGate.phase==2); // Held physical pedal cannot resume.
    physical=992;advance(400);return curve;
  };
  const auto buttonCurve=fullTrace(true),joystickCurve=fullTrace(false);
  assert(buttonCurve==joystickCurve);
  puts("Button 100% and full north have identical complete PWM ramps at their real HTTP cadences; held pedal cannot override or resume after STOP");
  // Every button level uses the same Node 2 demand conversion as the joystick.
  for(unsigned percent=1;percent<=100;++percent) {
    const size_t before=outgoing.size();button(1,percent);assert(server.code==200);
    for(unsigned seq=2;seq<=25;++seq) {advance(50);button(seq,percent);assert(drive.held);}
    const unsigned demand=(percent*255+50)/100,expected=62+193*demand/255;
    assert(pcm::remoteGate.phase==1 && pcm::stopReason==0 && (unsigned)pcm::pwm[13]==expected && pcm::pwm[14]==pcm::pwm[13]);
    for(size_t n=before;n<outgoing.size();++n) assert(outgoing[n].id!=0x301 && outgoing[n].id!=0x300);
    stopDrive();bus();advance(400);
  }
  joystick(1,255);
  for(unsigned seq=2;seq<=25;++seq) {advance(50);joystick(seq,255);assert(drive.held);}
  assert(pcm::pwm[13]==255);stopDrive();bus();advance(400);
  button(1,100);
  for(unsigned seq=2;seq<=25;++seq) {advance(50);button(seq,100);assert(drive.held);}
  assert(pcm::pwm[13]==255);
  puts("All 100 button levels use the joystick CAN path; button 100% equals full north, PWM255; no injected pedal/switch frames");
  stopDrive();bus();advance(400);
  // Clicking reverse alone must switch the relay with idle output, including
  // a completion ACK that arrives between browser renewals.
  for(bool requested : {true,false,true,false}) {
    const uint32_t token=drive.token;
    reverseButton(1,requested,token);assert(server.code==200);
    for(unsigned seq=2;seq<12 && drive.held;++seq) {advance(50);reverseButton(seq,requested,token);assert(server.code==200);}
    assert(!drive.held && pcm::reverseOut==requested && pcm::pwm[13]==62);
    advance(100);reverseButton(12,requested,token);assert(server.code==200 && !drive.held);
    assert(pcm::reverseOut==requested);advance(300);
  }
  // A physical direction change can land between browser status polls. A
  // throttle-only press must inherit the currently applied manual direction,
  // not silently restore an old direction cached by the preceding GET.
  assert(!drive.session);sendDriveState();assert(!drive.reverse);
  physicalSwitches=SWITCH_REVERSE;advance(200);assert(pcm::reverseOut);
  button(1,100);
  for(unsigned seq=2;seq<=25;++seq){advance(50);button(seq,100);assert(drive.held && pcm::reverseOut);}
  assert(pcm::pwm[13]==255);stopDrive();bus();advance(400);
  physicalSwitches=0;advance(200);sendDriveState();assert(!pcm::reverseOut);
  puts("Button throttle inherits the actual manual direction even when the physical reverse switch changes between HTTP polls");
  // Steering belongs to its separate button endpoint; releasing throttle does not stop it.
  steeringRelay=SteeringRelay{};steeringNode=SteeringControl{};
  advance(100);server.args={{"direction","left"},{"token",String(steeringRelay.token)},{"seq","1"}};
  handleSteeringCommand();bus();assert(steeringNode.direction==1);
  button(1,100);advance(50);stopDrive();bus();assert(steeringNode.direction==1);
  stopSteeringRelay();bus();advance(400);
  // Reverse while driving: first idle ACK, then direction ACK, then ramp.
  joystick(1,255);
  for(unsigned seq=2;seq<=25;++seq) {advance(50);joystick(seq,255);}
  assert(!pcm::reverseOut && pcm::pwm[13]==255);
  joystick(26,255,0,0,true);assert(drive.held && !pcm::reverseOut);
  for(unsigned seq=27;seq<=59;++seq) {advance(50);joystick(seq,255,0,0,true);assert(drive.held);}
  assert(pcm::reverseOut && pcm::pwm[13]==255);
  stopDrive();bus();advance(400);assert(pcm::pwm[13]==62 && pcm::reverseOut);
  puts("Reverse button and joystick pass actual relay switching, idle-before-change, applied direction ACK, delayed completion, rapid toggles and independent steering");
}

'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    cpp = path / 'control-link.cpp'
    cpp.write_text(shim + '\nvoid delay(unsigned n){clockMs+=n;}\nnamespace pcm {\nconstexpr int LOW=0,HIGH=1,OUTPUT=1;\n' + motor_io + motor + '\n}\n' + backend + scenarios)
    binary = path / 'control-link'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'node5_dash'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
