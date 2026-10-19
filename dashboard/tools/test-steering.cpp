#include <cassert>
#include <cstdio>
#include "SteeringControl.h"
void command(uint8_t *d, uint8_t dir, uint16_t seq, uint32_t token) {
  d[0]=dir; d[1]=dir ? 60 : 0; d[2]=seq; d[3]=seq>>8; steeringWrite32(d+4,token);
}
int main() {
  SteeringControl s; uint8_t d[8] = {};
  assert(!s.direction);
  command(d,1,1,s.token); assert(s.accept(d,8,100)); assert(s.direction==1);
  assert(!s.accept(d,8,200)); // Replay does not renew the lease.
  assert(!s.tick(449)); assert(s.tick(450)); assert(s.direction==0 && s.reason==2);
  assert(!s.accept(d,8,451)); assert(!s.direction); // Queued command from expired token.
  command(d,2,2,s.token); assert(s.accept(d,8,500));
  command(d,0,0,0); assert(s.accept(d,8,501)); assert(!s.direction);
  command(d,1,1,s.token); assert(s.accept(d,8,600));
  d[1]=255; assert(!s.accept(d,8,601)); assert(!s.direction && s.reason==3);
  command(d,1,1,s.token); assert(s.accept(d,8,700));
  command(d,2,2,s.token); assert(!s.accept(d,8,701)); assert(!s.direction); // Release before reversal.
  command(d,1,1,s.token); assert(s.accept(d,8,1000));
  const uint32_t token=s.token;
  for(uint16_t n=2;n<=50;++n) {command(d,1,n,token);assert(s.accept(d,8,1000+(n-1)*100));}
  command(d,1,51,token); assert(!s.accept(d,8,6000)); assert(!s.direction && s.reason==4);
  command(d,2,1,s.token); assert(s.accept(d,8,0xfffffff0));
  assert(!s.tick(100)); assert(s.tick(334)); assert(!s.direction); // millis rollover.
  command(d,1,1,s.token); assert(s.accept(d,8,500)); assert(!s.accept(d,7,510)); assert(!s.direction);
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
  puts("Steering state tests passed: release, replay, timeout, hold limit, invalid PWM, reversal, millis wrap");
}
