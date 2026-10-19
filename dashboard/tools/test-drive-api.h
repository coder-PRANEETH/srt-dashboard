// Native integration checks against the unchanged Node 5 HTTP/CAN functions.
void testDrive() {
  RemoteDriveGate pedal, switches;
  uint16_t physicalPedal=0; uint8_t physicalSwitches=0;
  size_t delivered=outgoing.size();
  auto bus=[&]() {
    while(delivered<outgoing.size()) {
      const auto f=outgoing[delivered++];
      if(f.id!=DRIVE_GATE_ID) continue;
      pedal.accept(f.data,f.len,1,physicalPedal<=DRIVE_IDLE_ADC,
        switches.phase==0 && switches.session==pedal.session,clockMs);
      switches.accept(f.data,f.len,3,f.data[0]==1 || bool(physicalSwitches&4)==switches.reverse,false,clockMs);
    }
    pedal.tick(clockMs,physicalPedal<=DRIVE_IDLE_ADC); switches.tick(clockMs);
    Input p{},s{}; p.id=0x121; s.id=0x122; p.len=s.len=8;
    pedal.status(p.data,physicalPedal); switches.status(s.data,physicalSwitches);
    incoming.push_back(p); incoming.push_back(s); readCan();
  };
  auto command=[&](const char *action,uint32_t seq=1,uint32_t percent=25) {
    server.args={{"action",action},{"seq",String(seq)},{"percent",String(percent)},{"token",String(drive.token)},{"reverse","1"}};
    handleDriveCommand();
  };
  clockMs=10000; outgoing.clear(); delivered=0; drive=DriveHost{};
  server.headers.clear(); command("throttle"); assert(server.code==403 && outgoing.empty());
  server.headers["X-SRT-Control"]="1";
  command("throttle"); assert(server.code==409 && !drive.held); // old firmware
  bus(); command("throttle",1,0); assert(server.code==400 && !drive.held);
  bus(); command("throttle"); assert(server.code==200 && drive.held && drive.adc==0);
  assert(outgoing.back().id==0x120); // never sends positive ADC before acknowledgements
  const auto session=drive.session;
  bus(); assert(pedal.phase==1 && switches.phase==1);
  for(int n=1;n<=12;++n) {
    clockMs+=100; bus(); command("throttle",n+1);
    assert(server.code==200);
    if(n<11) assert(drive.adc==0);
  }
  assert(drive.adc==1023);
  assert(outgoing.back().id==0x301 && outgoing.back().len==2 && outgoing.back().data[0]==255 && outgoing.back().data[1]==3);
  assert(outgoing[outgoing.size()-2].id==0x300 && outgoing[outgoing.size()-2].len==1);
  assert(!driveCanShift());
  command("reverse"); assert(server.code==409 && !drive.held && drive.adc==0);
  assert(drive.token!=session); bus(); assert(pedal.phase==2 && switches.phase==2);
  // A released/expired gate session cannot restart even with a later sequence.
  uint8_t old[8]={1,0,0,0,0,0,99,0}; driveWrite32(old+2,session);
  assert(!pedal.accept(old,8,1,true,false,clockMs));
  assert(!driveCanShift());
  for(int n=0;n<12;++n) { clockMs+=100; bus(); serviceDrive(); }
  bus(); serviceDrive(); bus(); serviceDrive();
  assert(pedal.phase==0 && switches.phase==0 && !drive.session);
  command("reverse"); assert(server.code==200 && drive.reverse);
  command("throttle",1,40); bus();
  for(int n=1;n<=24;++n) {
    clockMs+=100; bus(); command("throttle",n+1,40);
    assert(server.code==200);
    if(n<21) assert(drive.adc==0); // zero before and after reversal
  }
  assert(drive.adc==1638 && drive.wireReverse);
  assert(outgoing[outgoing.size()-2].data[0]&4);
  // Local pedal overrides terminate the remote gesture; never resume on release.
  physicalPedal=1000; clockMs+=100; bus(); serviceDrive();
  assert(!drive.held && drive.adc==0 && pedal.phase==2);
  physicalPedal=0; bus(); assert(!drive.held);
  clockMs+=1100; bus(); serviceDrive(); bus();
  assert(switches.phase==2 && pedal.phase==2); // physical reverse must match
  physicalSwitches=4;
  for(int n=0;n<4;++n) { clockMs+=100; bus(); serviceDrive(); }
  assert(switches.phase==0 && pedal.phase==0);
  command("throttle"); bus();
  clockMs+=350; bus(); serviceDrive();
  assert(!drive.held && pedal.phase==2 && switches.phase==2 && drive.adc==0);
  command("throttle"); bus(); // new intentional gesture can recover locked gates
  clockMs+=100; bus(); serviceDrive(); assert(drive.held);
  clockMs+=260; serviceDrive(); assert(!drive.held); // stale acknowledgement
  bus(); command("throttle"); txWorks=false; clockMs+=100; bus(); serviceDrive();
  assert(!drive.held && drive.adc==0); txWorks=true;
  assert(driveSwitchMask(127,false)==(2|8|16|32));
  assert(driveSwitchMask(127,true)==(2|4|8|16|32));
  // A partial claim cannot keep overwriting an unclaimed normal pedal sender.
  drive=DriveHost{}; pedal=RemoteDriveGate{}; switches=RemoteDriveGate{}; delivered=outgoing.size();
  physicalPedal=0; physicalSwitches=0; clockMs+=1000; bus();
  command("throttle"); physicalPedal=1200; bus(); // pedal rejects claim, switches accept
  assert(pedal.phase==0 && switches.phase==1);
  stopDrive(); bus(); assert(switches.phase==2);
  size_t before=outgoing.size(); clockMs+=100; bus(); serviceDrive();
  for(size_t n=before;n<outgoing.size();++n) assert(outgoing[n].id!=0x301);
  physicalPedal=0;
  for(int n=0;n<15;++n){clockMs+=100;bus();serviceDrive();}
  assert(pedal.phase==0 && switches.phase==0 && !drive.session);
  // A continuously renewed browser gesture still has a five-second hard limit.
  command("throttle"); bus();
  const uint32_t heldToken=drive.token;
  for(int n=1;n<50;++n){clockMs+=100;bus();command("throttle",n+1);assert(drive.held);}
  clockMs+=100;bus();serviceDrive();assert(!drive.held && drive.adc==0 && drive.token!=heldToken);
  bus(); command("throttle"); bus();
  command("throttle"); assert(server.code==409 && !drive.held); // repeated sequence
  // Time arithmetic and gate expiry across millis wrap.
  RemoteDriveGate wrap; old[6]=1; driveWrite32(old+2,99);
  assert(wrap.accept(old,8,1,true,false,0xFFFFFFF0));
  wrap.tick(100); assert(wrap.phase==1); wrap.tick(350); assert(wrap.phase==2);
  assert(!wrap.accept(old,8,1,true,false,400));
  old[0]=3; assert(!wrap.accept(old,8,1,true,false,400));
  assert(wrap.accept(old,8,1,true,true,400) && wrap.phase==0);
}
