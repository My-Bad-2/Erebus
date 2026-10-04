#pragma once

#include <cstdint>

namespace kernel::hw::apic {
enum class ApicMode : std::uint8_t { Disabled, xAPIC, x2APIC };

enum class DeliveryMode : std::uint8_t {
  Fixed = 0b000,
  LowestPriority = 0b001,
  SMI = 0b010,
  NMI = 0b100,
  INIT = 0b101,
  StartUp = 0b110,
  ExtINT = 0b111
};

enum class DestinationMode : std::uint8_t {
  Physical = 0,
  Logical = 1,
};

enum class TriggerMode : std::uint8_t {
  Edge = 0,
  Level = 1,
};

enum class PinPolarity : std::uint8_t {
  High = 0,
  Low = 1,
};

enum class TimerMode : std::uint8_t {
  OneShot = 0b00,
  Periodic = 0b01,
  TscDeadline = 0b10,
};

enum class DestinationShorthand : std::uint8_t {
  None = 0,
  Self = 1,
  AllIncludingSelf = 2,
  AllExcludingSelf = 3,
};

enum class TimerDivide : std::uint8_t {
  Div2 = 0b0000,
  Div4 = 0b0001,
  Div8 = 0b0010,
  Div16 = 0b0011,
  Div32 = 0b1000,
  Div64 = 0b1001,
  Div128 = 0b1010,
  Div1 = 0b1011,
};
} // namespace kernel::hw::apic