#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "SteeringControl.h"
#include "SteeringFeedback.h"
#include "../../steering_tx/WheelPitch.h"
void command(uint8_t *d, uint8_t dir, uint16_t seq, uint32_t token) {
  d[0]=dir; d[1]=dir ? 60 : 0; d[2]=seq; d[3]=seq>>8; steeringWrite32(d+4,token);
}
int main() {
  SteeringControl s; uint8_t d[8] = {};
  assert(!s.direction);
  command(d,1,1,s.token); assert(s.accept(d,8,99)); assert(s.direction==1 && !s.feedbackAvailable);
  s.stop();
  s.setFeedbackAvailable(true);
  command(d,1,1,s.token); assert(s.accept(d,8,100)); assert(s.direction==1);
  assert(!s.accept(d,8,200)); // Replay does not renew the lease.
  assert(!s.tick(449)); assert(s.tick(450)); assert(s.direction==0 && s.reason==2);
  assert(!s.accept(d,8,451)); assert(!s.direction); // Queued command from expired token.
  command(d,2,2,s.token); assert(s.accept(d,8,500));
  command(d,0,0,0); assert(s.accept(d,8,501)); assert(!s.direction);
  command(d,1,1,s.token); assert(s.accept(d,8,600));
  d[1]=0; assert(!s.accept(d,8,601)); assert(!s.direction && s.reason==3);
  for(int pwm=1;pwm<=255;++pwm) {
    command(d,1,pwm,s.token);d[1]=pwm;
    assert(s.accept(d,8,602) && s.pwm==pwm);
    uint8_t status[8];s.status(status);assert(status[1]==pwm && status[3]==2 && steeringOutputValid(status));
  }
  command(d,0,0,0);assert(s.accept(d,8,603));
  command(d,1,1,s.token); assert(s.accept(d,8,700));
  command(d,2,2,s.token); assert(!s.accept(d,8,701)); assert(!s.direction); // Release before reversal.
  command(d,1,1,s.token); assert(s.accept(d,8,1000));
  const uint32_t token=s.token;
  for(uint16_t n=2;n<=100;++n) {command(d,1,n,token);assert(s.accept(d,8,1000+(n-1)*100));}
  assert(s.direction==1 && s.token==token); // Continuous renewals pass the old 5 s cap.
  assert(!s.tick(11249)); assert(s.tick(11250)); assert(!s.direction && s.reason==2);
  command(d,2,1,s.token); assert(s.accept(d,8,0xfffffff0));
  assert(!s.tick(100)); assert(s.tick(334)); assert(!s.direction); // millis rollover.
  command(d,1,1,s.token); assert(s.accept(d,8,500)); assert(!s.accept(d,7,510)); assert(!s.direction);
  command(d,1,1,s.token); assert(s.accept(d,8,600));
  const auto oldToken=s.token;
  assert(s.setFeedbackAvailable(false)); assert(s.direction==1 && s.token==oldToken);
  s.setFeedbackAvailable(true); assert(!s.accept(d,8,601)); assert(s.direction==1); // Replay still rejected.
  command(d,0,0,0); s.setFeedbackAvailable(false); assert(s.accept(d,8,602));
  SteeringRelay r;
  assert(!r.accept(1,1,r.token,9,false,100));
  assert(r.accept(1,1,r.token,9,true,100));
  const auto lease=r.token;
  assert(!r.accept(1,1,lease,9,true,200));
  assert(!r.accept(2,2,lease,9,true,200));
  assert(!r.accept(1,2,lease,10,true,200));
  assert(r.tick(450)); assert(!r.accept(1,2,lease,9,true,451));
  assert(r.accept(2,1,r.token,9,true,500));
  r.stop(); assert(!r.direction); assert(!r.accept(2,2,lease,9,true,501));
  SteeringFeedback f;
  assert(!f.fresh(0)); f.status(d,0); assert(!d[3] && d[2]==255);
  IMUData packet={-45.25f,100};
  auto accept=[&](uint32_t now){return f.accept((const uint8_t*)&packet,sizeof(packet),now);};
  assert(accept(1000)); assert(f.angle==-45.25f && f.sequence==100 && f.fresh(1249));
  f.status(d,1025); assert(steeringAngleFrameValid(d) && steeringAngleTenths(d)==-453);
  assert(d[2]==25 && d[3]==1 && steeringAngleSequence(d)==100);
  assert(steeringAngleFrameFresh(d,224)); assert(!steeringAngleFrameFresh(d,225));
  assert(!accept(1100)); assert(f.receivedAt==1000); // Duplicate cannot keep link alive.
  packet.sequence=99; assert(!accept(1101));
  packet.sequence=101; packet.steeringAngle=NAN; assert(!accept(1102));
  packet.steeringAngle=INFINITY; assert(!accept(1103));
  packet.steeringAngle=270.1f; assert(!accept(1104));
  packet.steeringAngle=-270.1f; assert(!accept(1105));
  assert(!f.accept((const uint8_t*)&packet,7,1106)); assert(!f.accept(nullptr,8,1107));
  assert(f.receivedAt==1000 && !f.fresh(1250));
  f.status(d,1250); assert(!d[3] && d[2]==250 && !steeringAngleFrameFresh(d,0));
  packet={0,0}; assert(accept(1250)); assert(f.outages==1 && f.fresh(1250)); // Transmitter reboot.
  packet={180,1}; assert(accept(1251)); f.status(d,1251); assert(steeringAngleTenths(d)==1800);
  packet={-180,2}; assert(accept(1252)); f.status(d,1252); assert(steeringAngleTenths(d)==-1800);
  // Every representable CAN angle in the IMU sender's full range survives.
  for (int tenths=-2700; tenths<=2700; ++tenths) {
    packet.steeringAngle=tenths/10.0f; ++packet.sequence;
    assert(accept(1253)); f.status(d,1253);
    assert(steeringAngleFrameValid(d) && steeringAngleTenths(d)==tenths);
  }
  for (int tenths : {-2701,2701}) {
    const uint16_t encoded=(uint16_t)tenths;
    d[0]=encoded>>8; d[1]=encoded;
    assert(!steeringAngleFrameValid(d));
  }
  SteeringFeedback wrapped;
  SteeringFeedback hornFeedback;
  IMUDataWithHorn hornPacket={270,1,1};
  assert(hornFeedback.accept((const uint8_t*)&hornPacket,9,0));
  assert(hornFeedback.hornAvailable && hornFeedback.hornPressed && hornFeedback.angle==270);
  assert(hornFeedback.hornActive(1));
  hornPacket={-270,2,0}; assert(hornFeedback.accept((const uint8_t*)&hornPacket,9,10));
  assert(!hornFeedback.hornPressed && hornFeedback.angle==-270);
  assert(!hornFeedback.hornActive(10));
  hornPacket.sequence=3;hornPacket.hornPressed=2;
  assert(!hornFeedback.accept((const uint8_t*)&hornPacket,9,20));
  assert(!hornFeedback.accept((const uint8_t*)&hornPacket,10,20));
  packet={0,3};assert(hornFeedback.accept((const uint8_t*)&packet,8,30));
  assert(!hornFeedback.hornAvailable && !hornFeedback.hornPressed);
  assert(!hornFeedback.hornActive(30));
  packet={12.3f,0xFFFFFFFF}; assert(wrapped.accept((const uint8_t*)&packet,8,0xFFFFFFF0));
  packet.sequence=0; assert(wrapped.accept((const uint8_t*)&packet,8,100)); assert(wrapped.fresh(349)); assert(!wrapped.fresh(350));
  d[3]=2; assert(!steeringAngleFrameValid(d)); d[3]=1; d[0]=0x7F; assert(!steeringAngleFrameValid(d));
  SteeringFeedback pitchFeedback;
  IMUDataWithPitch pitchPacket={25,0,1,9,1};
  assert(pitchFeedback.accept((const uint8_t*)&pitchPacket,sizeof(pitchPacket),100));
  assert(pitchFeedback.pitchAvailable && pitchFeedback.pitch==9 && pitchFeedback.hornActive(100));
  pitchFeedback.pitchStatus(d,100);
  assert(wheelPitchFrameValid(d) && steeringAngleFrameFresh(d,0) && steeringAngleTenths(d)==90);
  assert(!steeringAngleFrameFresh(d,250));
  pitchFeedback.pitchStatus(d,350);assert(!d[3]);
  pitchPacket.sequence=1;pitchPacket.pitchAngle=NAN;
  assert(!pitchFeedback.accept((const uint8_t*)&pitchPacket,sizeof(pitchPacket),101));
  pitchPacket.pitchAngle=90.1f;
  assert(!pitchFeedback.accept((const uint8_t*)&pitchPacket,sizeof(pitchPacket),101));
  pitchPacket.pitchAngle=-9;pitchPacket.pitchValid=2;
  assert(!pitchFeedback.accept((const uint8_t*)&pitchPacket,sizeof(pitchPacket),101));
  pitchPacket.pitchValid=1;
  assert(pitchFeedback.accept((const uint8_t*)&pitchPacket,sizeof(pitchPacket),101));
  pitchFeedback.pitchStatus(d,101);assert(steeringAngleTenths(d)==-90);
  IMUDataWithHorn legacy={25,2,0};
  assert(pitchFeedback.accept((const uint8_t*)&legacy,sizeof(legacy),102));
  pitchFeedback.pitchStatus(d,102);assert(!d[3] && !pitchFeedback.pitchAvailable);
  float pitchDegrees=0;
  assert(measureWheelPitch(0,0,16384,pitchDegrees) && pitchDegrees==0);
  for(float angle : {-45.0f,-9.0f,9.0f,45.0f}) {
    const float rad=angle*0.01745329252f;
    assert(measureWheelPitch(lroundf(-sinf(rad)*16384),0,lroundf(cosf(rad)*16384),pitchDegrees));
    assert(fabsf(pitchDegrees-angle)<0.01f);
  }
  assert(!measureWheelPitch(0,0,0,pitchDegrees));
  assert(!measureWheelPitch(32767,0,0,pitchDegrees));
  HornControl hc;HornRelay hr;
  uint32_t hornNow=0xFFFFFF00;
  uint8_t h[8]={1,1,0,0,0,0,0,0};steeringWrite32(h+4,hc.token);
  for(unsigned n=1;n<140000;++n){
    const uint16_t seq=(n-1)%65535+1;hornNow+=80;
    assert(hr.accept(1,seq,hr.token,hc.token,true,hornNow));
    h[2]=seq;h[3]=seq>>8;assert(hc.accept(h,8,hornNow));assert(hc.held);
    assert(!hc.accept(h,8,hornNow)); // Duplicate doesn't refresh lease.
  }
  const auto oldHorn=hc.token;hc.tick(hornNow+350);assert(!hc.held && hc.token!=oldHorn);
  assert(!hc.accept(h,8,hornNow+350));
  h[0]=h[1]=0;assert(hc.accept(h,8,hornNow+350));
  h[0]=2;assert(!hc.accept(h,8,hornNow+350) && !hc.held);
  pitchFeedback.hornStatus(d,102);assert(wirelessHornFrameValid(d));
  d[0]=1;assert(!wirelessHornFrameValid(d));d[1]=4;assert(wirelessHornFrameValid(d));
  d[1]=2;assert(!wirelessHornFrameValid(d));d[1]=3;d[2]=250;assert(!wirelessHornFrameValid(d));
  SteeringFeedback invalidAngle;
  IMUDataWithHornValidity onlyHorn={25,1,1,0};
  assert(invalidAngle.accept((const uint8_t*)&onlyHorn,sizeof(onlyHorn),100));
  assert(!invalidAngle.fresh(100) && invalidAngle.hornActive(100));
  invalidAngle.status(d,100);assert(!d[3]);
  invalidAngle.hornStatus(d,100);assert(wirelessHornFrameValid(d) && d[0]==1 && d[1]==3);
  assert(!invalidAngle.accept((const uint8_t*)&onlyHorn,sizeof(onlyHorn),101));
  onlyHorn.sequence=0;assert(!invalidAngle.accept((const uint8_t*)&onlyHorn,sizeof(onlyHorn),101));
  onlyHorn.sequence=2;onlyHorn.angleValid=2;assert(!invalidAngle.accept((const uint8_t*)&onlyHorn,sizeof(onlyHorn),102));
  onlyHorn.angleValid=0;onlyHorn.hornPressed=0;assert(invalidAngle.accept((const uint8_t*)&onlyHorn,sizeof(onlyHorn),103));
  assert(!invalidAngle.hornActive(103) && !invalidAngle.fresh(103));
  onlyHorn.sequence=3;onlyHorn.angleValid=1;onlyHorn.hornPressed=1;
  assert(invalidAngle.accept((const uint8_t*)&onlyHorn,sizeof(onlyHorn),104) && invalidAngle.fresh(104));
  assert(!invalidAngle.hornActive(354));
  puts("Horn lease passed: 139999 renewals, repeated sequence rollover, millis wrap, no total hold limit, expiry, replay, STOP, malformed frames and status flags");
  puts("Wheel pitch passed: 14-byte ABI, old packet compatibility, signed CAN angle, validity/age, corrupt fields and known +/-9 degree accelerometer inputs");
  puts("Steering state tests passed: release, replay, renewable continuous hold, timeout, invalid PWM, reversal, millis wrap");
  puts("Wireless angle tests passed: packet ABI, signed CAN angle, age, validation, duplicates, sequence/millis wrap, reboot and feedback interlock");
}
