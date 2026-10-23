// Exercise HTTP validation/ownership/expiry. The actual Node 2 transfer and
// relay logic are covered separately by test-control-link.py.
void testDrive() {
  RemoteDriveGate pedal,switches,motor;
  uint16_t physicalPedal=930; uint8_t physicalSwitches=0,duty=DRIVE_REST_PWM;
  size_t delivered=outgoing.size();
  auto bus=[&]() {
    while(delivered<outgoing.size()) {
      const auto f=outgoing[delivered++];
      if(f.id==DRIVE_GATE_ID) {
        const bool handback=switches.phase==0;
        pedal.accept(f.data,f.len,1,physicalPedal<=DRIVE_IDLE_ADC,handback && switches.session==pedal.session,clockMs);
        motor.accept(f.data,f.len,1,physicalPedal<=DRIVE_IDLE_ADC,handback && switches.session==motor.session,clockMs);
        switches.accept(f.data,f.len,3,f.data[0]==1 || bool(physicalSwitches&SWITCH_REVERSE)==switches.reverse,false,clockMs);
      } else if(f.id==DRIVE_PWM_ID && motor.phase==1) duty=driveThrottleDuty(f.data[0]);
      assert(f.id!=0x301 && f.id!=0x300); // Host never impersonates physical inputs.
    }
    pedal.tick(clockMs);switches.tick(clockMs);motor.tick(clockMs);
    if(motor.phase!=1)duty=DRIVE_REST_PWM;
    Input p{},s{},m{},out{};p.id=0x121;s.id=0x122;m.id=0x124;out.id=0x201;p.len=s.len=m.len=out.len=8;
    pedal.status(p.data,physicalPedal);switches.status(s.data,physicalSwitches);motor.status(m.data,duty);
    out.data[2]=motor.reverse;
    incoming.push_back(p);incoming.push_back(s);incoming.push_back(m);incoming.push_back(out);readCan();
  };
  auto command=[&](const char *action,uint32_t seq=1,uint32_t percent=25) {
    server.args={{"action",action},{"seq",String(seq)},{"percent",String(percent)},{"token",String(drive.token)},{"reverse","1"}};
    handleDriveCommand();
  };
  auto handback=[&]() {for(int n=0;n<6;++n){clockMs+=50;bus();serviceDrive();}bus();};
  clockMs=10000;outgoing.clear();delivered=0;drive=DriveHost{};
  server.headers.clear();command("throttle");assert(server.code==403 && outgoing.empty());
  server.headers["X-SRT-Control"]="1";command("throttle");assert(server.code==409 && !drive.held);
  bus();command("throttle",1,0);assert(server.code==400 && !drive.held);
  bus();command("throttle");assert(server.code==200 && drive.held && drive.pwm==64);
  const auto session=drive.session;bus();assert(pedal.phase==1 && switches.phase==1 && motor.phase==1);
  for(int n=2;n<6;++n){clockMs+=50;bus();command("throttle",n);assert(server.code==200);bus();}
  assert(duty==driveThrottleDuty(64) && !driveCanShift());
  // Physical input cannot take over while remote owns the output.
  physicalPedal=4095;clockMs+=50;bus();command("throttle",6);bus();assert(drive.held && duty==110);
  command("throttle",6);bus();assert(server.code==409 && !drive.held && motor.phase==2 && duty==62);
  uint8_t old[8]={1,2,0,0,0,0,99,0};driveWrite32(old+2,session);
  assert(!pedal.accept(old,8,1,true,false,clockMs));
  handback();assert(pedal.phase==2);physicalPedal=930;handback();assert(!drive.session && pedal.phase==0 && motor.phase==0);
  // Reverse alone is a renewable zero-demand transaction, then idle completion.
  command("reverse");bus();assert(server.code==200 && drive.held && drive.shiftOnly);
  for(int seq=2;seq<10 && drive.held;++seq){clockMs+=50;bus();command("reverse",seq);bus();}
  assert(!drive.held && motor.reverse && duty==62);handback();assert(pedal.phase==2);
  physicalSwitches=SWITCH_REVERSE;handback();assert(!drive.session && pedal.phase==0);
  command("throttle");bus();clockMs+=350;bus();serviceDrive();bus();assert(!drive.held && duty==62 && pedal.phase==2);
  command("throttle");bus();clockMs+=50;bus();serviceDrive();assert(drive.held); // Intentional new gesture.
  clockMs+=260;serviceDrive();assert(!drive.held); // Stale CAN cannot renew.
  handback();bus();command("throttle");bus();txWorks=false;clockMs+=50;bus();serviceDrive();assert(!drive.held);txWorks=true;
  handback();
  // Partial ownership must never inject a false pedal to complete its own claim.
  drive=DriveHost{};pedal=RemoteDriveGate{};switches=RemoteDriveGate{};motor=RemoteDriveGate{};
  delivered=outgoing.size();physicalPedal=930;physicalSwitches=0;clockMs+=1000;bus();command("throttle");
  physicalPedal=1200;bus();assert(pedal.phase==0 && switches.phase==1 && motor.phase==0);
  stopDrive();bus();assert(switches.phase==2);physicalPedal=930;handback();assert(!drive.session);
  command("throttle");bus();const auto heldToken=drive.token;
  for(int n=1;n<50;++n){clockMs+=100;bus();command("throttle",n+1);assert(drive.held);}
  clockMs+=100;bus();serviceDrive();assert(!drive.held && drive.token!=heldToken);
  // Time arithmetic and released-session rejection across millis wrap.
  RemoteDriveGate wrap;old[6]=1;driveWrite32(old+2,99);
  assert(wrap.accept(old,8,1,true,false,0xFFFFFFF0));wrap.tick(100);assert(wrap.phase==1);
  wrap.tick(350);assert(wrap.phase==2);assert(!wrap.accept(old,8,1,true,false,400));
  old[0]=3;assert(!wrap.accept(old,8,1,true,false,400));assert(wrap.accept(old,8,1,true,true,400) && wrap.phase==0);
  assert(driveSwitchMask(63,false)==(2|8|16));assert(driveSwitchMask(63,true)==(2|8|16|32));
}
