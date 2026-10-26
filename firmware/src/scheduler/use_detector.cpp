#include "scheduler/use_detector.h"

bool UseDetector::update(uint32_t now, const UseSignals &signals) {
  const bool lowAlone = signals.jet1Speed == 1 && !signals.heating && !signals.filtering;
  if (!lowAlone) {
    lowSince_ = 0;
  } else if (!lowSince_) {
    lowSince_ = now;
  }
  inUse_ = signals.jet1Speed == 2 || signals.jet2Speed > 0 || signals.blower || signals.light ||
           (lowSince_ && now - lowSince_ >= LowJetsSec);
  return inUse_;
}
