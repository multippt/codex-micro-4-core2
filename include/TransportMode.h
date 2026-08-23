// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

enum class TransportMode : uint8_t { Bluetooth = 0, Usb = 1 };

inline TransportMode normalizeTransportMode(uint8_t value) {
  return value == static_cast<uint8_t>(TransportMode::Usb)
             ? TransportMode::Usb
             : TransportMode::Bluetooth;
}
