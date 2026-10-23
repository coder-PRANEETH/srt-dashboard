#pragma once
#include <stdint.h>

// Node 3 / 0x300 DLC 1. A set bit means the active-low switch is ON.
constexpr uint8_t SWITCH_COUNT = 6;
constexpr uint8_t SWITCH_CRUISE = 1U << 0;
constexpr uint8_t SWITCH_HILL_HOLD = 1U << 1;
constexpr uint8_t SWITCH_AUTONOMOUS = 1U << 2;
constexpr uint8_t SWITCH_REMOTE_MODE = 1U << 3;
constexpr uint8_t SWITCH_HORN = 1U << 4;
constexpr uint8_t SWITCH_REVERSE = 1U << 5;
constexpr uint8_t SWITCH_VALID_MASK = (1U << SWITCH_COUNT) - 1;
