#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include <deque>

#include <ArduinoJson.h>

// How fast this tub heats and cools, learned from its own readings.
//
// Water temperature follows dT/dt = heaterRate (while heating) - lossCoeff * (water - outside), so:
//   - heaterRate is the °F/hr the heater adds before losses; seeded from heater kW and tub volume.
//   - lossCoeff is the fraction of the water-to-outside difference lost per hour; seeded at a typical
//     insulated tub with its cover on.
// Each learns from measured runs: heating runs give heaterRate (after adding back the losses), and runs with
// the heater off give lossCoeff. Runs while the tub is in use (cover likely off) are skipped.
struct ThermalObservation {
  uint32_t end = 0;  // Unix time the run ended
  float value = NAN;  // heat: heaterRate in °F/hr; cool: lossCoeff per hour
  float startTemp = NAN;
  float endTemp = NAN;
  float outside = NAN;  // average over the run; NAN if no forecast was available
  uint16_t minutes = 0;
};

struct ThermalSample {
  uint32_t now = 0;  // Unix time
  float waterTemp = NAN;  // NAN while the spa hasn't reported one
  bool heating = false;
  bool inUse = false;  // jets on high, jet 2, blower or light: someone is probably in with the cover off
  float outside = NAN;  // NAN when no forecast covers now
};

class ThermalModel {
 public:
  static constexpr size_t MaxObservations = 30;
  static constexpr uint32_t UseGraceSec = 30 * 60;  // treat the cover as off this long after the last use
  static constexpr uint32_t MinHeatRunSec = 20 * 60;
  static constexpr float MinHeatRiseF = 2.0f;
  static constexpr uint32_t MinCoolRunSec = 3 * 3600;
  static constexpr uint32_t MaxCoolRunSec = 8 * 3600;  // long coasts are split so they yield more readings
  static constexpr uint32_t MaxSampleGapSec = 5 * 60;  // longer silences (reboot, lost link) restart the runs
  static constexpr float FallbackOutsideF = 50.0f;  // for heating runs measured without a forecast
  static constexpr float DefaultLossCoeff = 0.012f;  // ~0.7°F/hr at 100°F water and 40°F outside

  // Seeds used until enough runs have been measured.
  void setSeeds(float heaterKw, uint16_t gallons);
  // Called every few seconds with the spa's latest status.
  void observe(const ThermalSample &sample);

  float heaterRate() const;
  float lossCoeff() const;
  float heaterRateSeed() const { return heaterRateSeed_; }
  float lossCoeffSeed() const { return DefaultLossCoeff; }

  // Net °F/hr while heating at `water` with `outside` around it; outside falls back when NAN.
  float heatingRate(float water, float outside) const;
  // °F/hr the water drops with the heater off (positive when cooling).
  float coolingRate(float water, float outside) const;
  // Hours to heat from `from` to `to`; 0 if already there, INFINITY if losses outrun the heater.
  float hoursToHeat(float from, float to, float outside) const;

  const std::deque<ThermalObservation> &heatRuns() const { return heat_; }
  const std::deque<ThermalObservation> &coolRuns() const { return cool_; }
  void load(const std::deque<ThermalObservation> &heat, const std::deque<ThermalObservation> &cool);
  void toJson(JsonObject out) const;
  void fromJson(JsonVariantConst in);
  bool dirty() const { return dirty_; }
  void clearDirty() { dirty_ = false; }

 private:
  // A reading edge: the moment the reported temperature changed. Timing runs edge to edge sidesteps the
  // spa's whole-degree resolution.
  struct Edge {
    uint32_t at = 0;
    float temp = NAN;
  };

  struct Run {
    bool active = false;
    uint32_t startedAt = 0;
    Edge first;
    Edge last;
    double outsideSum = 0;
    uint32_t outsideCount = 0;
  };

  void startRun(Run &run, uint32_t now);
  void addEdge(Run &run, uint32_t now, float temp);
  float runOutside(const Run &run) const;
  void finishHeatRun();
  void finishCoolRun(bool restart);
  void addObservation(std::deque<ThermalObservation> &list, const ThermalObservation &obs);

  float heaterRateSeed_ = 11.0f;
  std::deque<ThermalObservation> heat_;  // oldest first
  std::deque<ThermalObservation> cool_;
  bool dirty_ = false;

  uint32_t lastSampleAt_ = 0;
  uint32_t lastUseAt_ = 0;
  float lastTemp_ = NAN;
  Run heatRun_;
  Run coolRun_;
};
