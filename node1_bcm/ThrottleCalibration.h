#pragma once
#include <stdint.h>

constexpr float THROTTLE_IDLE_VOLTS = 0.7f;
constexpr float THROTTLE_ADC_VOLTS = 3.3f;
inline float throttleRawVoltage(uint16_t adc) { return adc * (THROTTLE_ADC_VOLTS / 4095.0f); }
inline float throttleVoltage(uint16_t adc) {
  const float corrected = throttleRawVoltage(adc) - THROTTLE_IDLE_VOLTS;
  return corrected > 0 ? corrected : 0.0f;
}
inline unsigned throttlePercent(uint16_t adc) {
  const unsigned percent = throttleVoltage(adc) * 100.0f / (THROTTLE_ADC_VOLTS - THROTTLE_IDLE_VOLTS);
  return percent > 100 ? 100 : percent;
}
