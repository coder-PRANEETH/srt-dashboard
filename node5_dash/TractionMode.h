#pragma once
#include <stdint.h>

constexpr uint32_t TRACTION_MODE_COMMAND_ID=0x125, TRACTION_MODE_STATUS_ID=0x126;
constexpr uint8_t TRACTION_MODE_VERSION=1;
inline uint32_t tractionModeToken(const uint8_t *d) {
  return (uint32_t)d[4] | ((uint32_t)d[5]<<8) | ((uint32_t)d[6]<<16) | ((uint32_t)d[7]<<24);
}
inline bool tractionModeFrameValid(const uint8_t *d) {
  return d[0]<=1 && d[1]==TRACTION_MODE_VERSION && d[3]<=11 && tractionModeToken(d)!=0;
}
struct TractionMode {
  bool mobile=true;
  uint32_t token=1;
  bool accept(const uint8_t *d,uint8_t len) {
    if(len!=8 || d[0]>1 || d[1]!=TRACTION_MODE_VERSION || d[2] || d[3]) return false;
    if(d[0] && tractionModeToken(d)!=token) return false;
    if(mobile!=bool(d[0])) {
      mobile=d[0]; if(++token==0) ++token;
    }
    return true;
  }
  void status(uint8_t *d,uint8_t pwm,uint8_t reason) const {
    d[0]=mobile;d[1]=TRACTION_MODE_VERSION;d[2]=pwm;d[3]=reason;
    for(uint8_t n=0;n<4;++n)d[4+n]=token>>(8*n);
  }
};
