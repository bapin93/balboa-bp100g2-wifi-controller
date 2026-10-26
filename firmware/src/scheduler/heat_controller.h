#pragma once

#include <math.h>
#include <stdint.h>

#include <functional>
#include <vector>

#include <ArduinoJson.h>

#include "scheduler/heat_planner.h"

// What the planner needs besides the water temperature. Rebuilt each tick from settings, the learned
// model, the forecast and the usage model.
struct PlanContext {
  TouSchedule tou;
  EnergyRates rates;
  float heaterKw = 5.5f;
  float heaterRate = 6.0f;
  float lossCoeff = 0.012f;
  float minTemp = 90;  // never planned or set below this
  bool autoEnabled = false;  // keep the tub ready by usage when no soak is planned
  bool lowRange = false;  // the spa is in its low temp range: the owner wants it left to cool, so stand aside
  std::function<float(uint32_t)> outsideAt = [](uint32_t) { return NAN; };
  // Hours of heat-up the owner will wait at a given time (from the usage model); NAN = no requirement.
  std::function<float(uint32_t)> readyWithinHours = [](uint32_t) { return NAN; };
};

struct ControllerTick {
  uint32_t now = 0;  // Unix time
  float setTemp = NAN;  // as the spa reports it
  float waterTemp = NAN;  // the live reading, or an estimate from the last one
  bool inUse = false;
};

// Decides the spa's set temperature.
//   - Auto: with nothing else going on, keeps the water warm enough to reach the comfort temp (the last one
//     set by hand) within the wait the usage model allows for each upcoming half hour, as cheaply as the
//     TOU prices and forecast allow.
//   - Soak: follows the cheapest plan to be at the target by the ready time (or heats straight through when
//     it's as soon as possible), then holds it for the session.
//   - Manual: a set temp change the controller didn't make (from the app or the spa's own panel) means
//     someone took over; a soak is dropped and nothing is sent until the hold ends.
// In the spa's low temp range it does nothing at all, and the range switch doesn't count as a manual change.
// Nothing ever lowers the set temp while the tub is in use.
class HeatController {
 public:
  static constexpr uint32_t StepSec = 900;
  static constexpr uint32_t MaxAheadSec = 30 * 3600;
  static constexpr uint32_t CommandSettleSec = 90;  // time for the spa to show a set temp we sent
  static constexpr uint32_t RestoreTrySec = 10 * 60;
  static constexpr uint32_t SoakAfterUseSec = 30 * 60;  // a soak runs on this long after the last use
  static constexpr uint32_t ManualAfterUseSec = 60 * 60;
  static constexpr uint32_t ManualMaxSec = 4 * 3600;  // when the tub is never used during the hold
  static constexpr float CoastMarginF = 2.0f;  // coast set temp sits this far under the plan's low point

  // Returns the set temp to send now, or NAN for none. Call about once a second.
  float tick(const ControllerTick &tick, const PlanContext &context);
  // The glue calls this once a set temp from tick() was queued to the spa.
  void commandSent(float setTemp, uint32_t now);

  // readyAt 0 means as soon as possible. Returns an error message, or nullptr when scheduled.
  const char *scheduleSoak(float target, uint32_t readyAt, float hours, const ControllerTick &tick,
                           const PlanContext &context);
  void cancelSoak(const ControllerTick &tick);
  void endManual();

  bool soakActive() const { return soak_.active; }
  bool manualActive() const { return manual_.active; }
  float comfortTemp() const { return comfort_; }
  // Unix time of the last sign someone means to get in: a raise of the set temp by hand, Heat Now, or a
  // planned soak reaching its ready time.
  uint32_t lastIntentAt() const { return lastIntentAt_; }
  void writeStateJson(JsonObject out, uint32_t now) const;
  void writePlanJson(JsonObject out) const;

  void toJson(JsonObject out) const;  // what survives a reboot
  void fromJson(JsonVariantConst in);
  bool dirty() const { return dirty_; }
  void clearDirty() { dirty_ = false; }

 private:
  struct Soak {
    bool active = false;
    float target = NAN;
    uint32_t readyAt = 0;
    float hours = 2;
    bool asap = false;
    float restoreTemp = NAN;  // set temp to go back to afterwards, when Auto is off
    uint32_t lastUse = 0;
  };

  struct Manual {
    bool active = false;
    uint32_t since = 0;
    float temp = NAN;
    uint32_t lastUse = 0;
  };

  void noteSetTemp(const ControllerTick &tick);
  uint32_t soakEnd() const;
  uint32_t manualUntil() const;
  void endSoak(uint32_t now);
  void replan(const ControllerTick &tick, const PlanContext &context);
  float setpointFromPlan(uint32_t now, const PlanContext &context) const;
  size_t stepAt(uint32_t now) const;
  void startRestore(float temp, uint32_t now);
  void writeNextHeat(JsonObject out, uint32_t now) const;

  Soak soak_;
  Manual manual_;
  float comfort_ = NAN;  // the last temp set by hand
  uint32_t lastIntentAt_ = 0;
  bool soakArrived_ = false;  // the current soak's ready time has been counted as intent
  bool autoEnabled_ = false;  // as of the last tick
  bool lowRange_ = false;
  float lastSeenSet_ = NAN;
  float pendingSet_ = NAN;
  uint32_t pendingAt_ = 0;
  float restoreTemp_ = NAN;
  uint32_t restoreUntil_ = 0;
  float readyInHours_ = NAN;  // heat-up time to the comfort temp from the water now
  PlanResult plan_;
  std::vector<float> planMin_;  // each step's requirement and price, for the app's chart
  std::vector<float> planPrice_;
  // Auto only, for the app's savings estimate: the same hours holding the comfort temp throughout, and this
  // plan made to end at the comfort temp too, so neither side gets credit for heat left to the next window.
  PlanResult holdPlan_;
  PlanResult fairPlan_;
  uint32_t planStart_ = 0;
  float planTarget_ = NAN;
  bool replanNeeded_ = true;
  bool dirty_ = false;
};
