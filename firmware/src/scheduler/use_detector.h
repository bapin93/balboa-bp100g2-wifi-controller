#pragma once

#include <stdint.h>

// What the spa reports that hints someone is in the tub.
struct UseSignals {
  uint8_t jet1Speed = 0;  // 0 off, 1 low, 2 high
  uint8_t jet2Speed = 0;
  bool blower = false;
  bool light = false;
  bool heating = false;  // heating or waiting to heat: the spa runs pump 1 low itself
  bool filtering = false;  // a filter cycle is running: same
};

// Decides whether someone is probably in the tub (cover off). Jets on high, jet 2, the blower and the light
// are always someone's doing. Pump 1 on low is too, but only once it has run a while with no filter cycle
// or heating to explain it: the spa runs it briefly by itself to check the water temperature.
class UseDetector {
 public:
  static constexpr uint32_t LowJetsSec = 10 * 60;

  bool update(uint32_t now, const UseSignals &signals);
  bool inUse() const { return inUse_; }

 private:
  uint32_t lowSince_ = 0;
  bool inUse_ = false;
};
