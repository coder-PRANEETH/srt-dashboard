#pragma once
#include <stdint.h>
#include <math.h>

// Sensor-relative pitch at the configured +/-2 g accelerometer range.
// Rotation of the wheel changes this reference; use for display only.
inline bool measureWheelPitch(int16_t ax,int16_t ay,int16_t az,float &degrees) {
  const float x=ax/16384.0f,y=ay/16384.0f,z=az/16384.0f;
  const float magnitude=sqrtf(x*x+y*y+z*z);
  if(!isfinite(magnitude) || magnitude<0.8f || magnitude>1.2f)return false;
  degrees=atan2f(-x,sqrtf(y*y+z*z))*57.2957795f;
  return isfinite(degrees);
}
