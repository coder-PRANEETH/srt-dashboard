// Actual Node 5 unified endpoint with real gate/steering state machines on a fake bus.
void testJoystick() {
  RemoteDriveGate pedalGate,switchGate,motorGate;
  SteeringControl steeringNode; // No angle sensor: motor-speed control still works.
  uint16_t physical=0;uint8_t physicalSwitches=SWITCH_REMOTE_MODE|SWITCH_AUTONOMOUS,motorDuty=DRIVE_REST_PWM;uint16_t lastMotorSeq=0;
  size_t delivered=outgoing.size();bool motorClaimed=false;
  auto bus=[&]() {
    while(delivered<outgoing.size()) {
      const auto f=outgoing[delivered++];
      if(f.id==DRIVE_GATE_ID) {
        pedalGate.accept(f.data,f.len,1,physical<=DRIVE_IDLE_ADC,switchGate.phase==0 && switchGate.session==pedalGate.session,clockMs);
        switchGate.accept(f.data,f.len,3,f.data[0]==1 || bool(physicalSwitches&SWITCH_REVERSE)==switchGate.reverse,false,clockMs);
        if((f.data[1]&2)||f.data[0]!=1) {
          motorGate.accept(f.data,f.len,1,physical<=DRIVE_IDLE_ADC,switchGate.phase==0 && switchGate.session==motorGate.session,clockMs);
          motorClaimed=motorGate.phase==1;
        }
      } else if(f.id==DRIVE_PWM_ID && motorGate.phase==1) {
        assert(f.len==8 && driveRead32(f.data+4)==motorGate.session && f.data[1]<=1);
        const uint16_t next=le16(f.data+2);assert(next>lastMotorSeq);lastMotorSeq=next;motorDuty=driveThrottleDuty(f.data[0]);
      } else if(f.id==STEERING_COMMAND_ID) steeringNode.accept(f.data,f.len,clockMs);
    }
    pedalGate.tick(clockMs);switchGate.tick(clockMs);motorGate.tick(clockMs);steeringNode.tick(clockMs);
    Input p{},s{},m{},st{},angle{},out{};p.id=0x121;s.id=0x122;m.id=0x124;st.id=0x111;angle.id=0x100;out.id=0x201;
    p.len=s.len=m.len=st.len=angle.len=out.len=8;
    pedalGate.status(p.data,physical);switchGate.status(s.data,physicalSwitches);motorGate.status(m.data,motorDuty);
    steeringNode.status(st.data);angle.data[3]=0;angle.data[2]=255;
    out.data[2]=motorGate.reverse;out.data[7]=0;
    push(0x300,{physicalSwitches});
    incoming.push_back(p);incoming.push_back(s);incoming.push_back(m);incoming.push_back(st);incoming.push_back(angle);incoming.push_back(out);readCan();
  };
  auto command=[&](uint16_t seq,uint8_t throttle,uint8_t direction,uint8_t pwm,bool reverse=false) {
    server.args={{"action","move"},{"token",String(drive.token)},{"seq",String(seq)},{"throttle",String(throttle)},
      {"direction",String(direction)},{"steering",String(pwm)},{"reverse",reverse?"1":"0"}};
    handleJoystickCommand();
  };
  drive=DriveHost{};steeringRelay=SteeringRelay{};joystickSteerStopping=false;
  clockMs=30000;incoming.clear();bus();server.headers.clear();
  command(1,180,2,180);assert(server.code==403 && !drive.held);
  server.headers["X-SRT-Control"]="1";bus();command(1,180,2,180);assert(server.code==200 && drive.held);
  assert(!drive.wireReverse); // Autonomous is bit 2; only bit 5 requests reverse.
  bus();assert(pedalGate.phase==1 && switchGate.phase==1 && motorGate.phase==1);
  clockMs+=50;bus();command(2,180,2,180);bus();assert(motorClaimed && motorDuty==DRIVE_REST_PWM); // First packet primes zero.
  clockMs+=50;bus();command(3,180,2,180);bus();assert(motorDuty==driveThrottleDuty(180) && steeringNode.direction==2 && steeringNode.pwm==180);
  // Continuously changing command values keeps one session; physical pedal is ignored.
  physical=4095;clockMs+=50;bus();command(4,91,2,42);bus();
  assert(server.code==200 && motorDuty==driveThrottleDuty(91) && steeringNode.pwm==42 && pedalGate.phase==1 && motorGate.phase==1);
  // Crossing left/right first sends STOP and waits for a new stopped Node 1 token.
  clockMs+=50;bus();command(5,0,1,255);assert(joystickSteerStopping);bus();assert(!steeringNode.direction);
  clockMs+=50;bus();command(6,0,1,255);bus();assert(steeringNode.direction==1 && steeringNode.pwm==255);
  // Duplicate packets stop both axes and cannot resume a held session.
  const auto token=drive.token;command(6,255,2,255);bus();
  assert(server.code==409 && !drive.held && motorGate.phase==2 && !steeringNode.direction && drive.token!=token);
  clockMs+=100;bus();serviceDrive();bus();assert(pedalGate.phase==2 && motorGate.phase==2);
  // Returning to manual waits for the physical pedal and all three handoff ACKs.
  physical=0;
  for(int n=0;n<20;++n){clockMs+=100;bus();serviceDrive();}
  bus();serviceDrive();assert(!drive.session && motorGate.phase==0 && pedalGate.phase==0);
  lastMotorSeq=0;clockMs+=50;bus();command(1,255,1,60);bus();
  clockMs+=50;bus();command(2,255,1,60);bus();clockMs+=50;bus();command(3,255,1,60);bus();
  assert(drive.held && motorDuty==255);
  clockMs+=350;bus();serviceDrive();bus();assert(!drive.held && motorGate.phase==2 && !steeringNode.direction);
  // Missing modern Node 2 or Node 1 capability cannot enable joystick output.
  drive=DriveHost{};sampleFor(0x124)->seen=false;command(1,255,1,255);assert(server.code==409 && !drive.held);
  server.args["throttle"]="256";handleJoystickCommand();assert(server.code==400 && !drive.held);
  puts("Joystick HTTP/CAN tests passed: unified variable PWM, all-three-node ownership, zero priming, pedal blocking, steering reversal ACK, replay, expiry, capability checks and idle-only handback");
}
