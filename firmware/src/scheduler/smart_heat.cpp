#include "scheduler/smart_heat.h"

#include <algorithm>
#include <ctime>

namespace {

constexpr uint32_t TickMs = 1000;
constexpr uint32_t ClockValidAfter = 1700000000;

struct LocalSlot {
  uint32_t date;  // YYYYMMDD
  int slot;  // half hour of the day, 0-47
  bool weekend;  // weekends and the utility's holidays
};

LocalSlot localSlot(uint32_t epoch, HolidaySet holidays) {
  const time_t at = static_cast<time_t>(epoch);
  tm local;
  localtime_r(&at, &local);
  const CivilDate date{local.tm_year + 1900, local.tm_mon + 1, local.tm_mday};
  LocalSlot out;
  out.date = static_cast<uint32_t>(date.year * 10000 + date.month * 100 + date.day);
  out.slot = local.tm_hour * 2 + local.tm_min / 30;
  out.weekend = local.tm_wday == 0 || local.tm_wday == 6 || isHoliday(holidays, date);
  return out;
}

}  // namespace

SmartHeat::SmartHeat(Balboa::SpaController &spa, Store &store, const DeviceConfig &config,
                     const ThermalModel &thermal, const WeatherClient &weather)
    : spa_(spa), store_(store), config_(config), thermal_(thermal), weather_(weather) {}

void SmartHeat::begin() {
  std::lock_guard<std::mutex> lock(mutex_);
  store_.loadSmart(controller_);
  store_.loadUsage(usage_);
  seenIntentAt_ = controller_.lastIntentAt();
}

void SmartHeat::loop() {
  const uint32_t nowMs = millis();
  if (nowMs - lastTickMs_ < TickMs) {
    return;
  }
  lastTickMs_ = nowMs;
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  if (now < ClockValidAfter || !spa_.hasFreshState()) {
    return;  // never act on a stale picture of the spa
  }
  const Balboa::SpaState &state = spa_.state();
  UseSignals signals;
  signals.jet1Speed = state.jet1Speed;
  signals.jet2Speed = config_.hideJet2 ? 0 : state.jet2Speed;
  signals.blower = state.blower;
  signals.light = state.light;
  signals.heating = state.heating || state.heatPending;
  signals.filtering = state.activeFilterCycle != 0;
  inUse_ = detector_.update(now, signals);
  std::lock_guard<std::mutex> lock(mutex_);
  const ControllerTick tick = makeTick(now);
  const uint32_t tickStartMs = millis();
  const float setTemp = controller_.tick(tick, makeContext());
  tickMaxMs_ = std::max<uint32_t>(tickMaxMs_, millis() - tickStartMs);
  if (!isnan(setTemp)) {
    JsonDocument doc;
    doc["action"] = "setTemp";
    doc["value"] = static_cast<int>(setTemp);
    const Balboa::CommandResult result = spa_.handleCommand(doc.as<JsonVariantConst>());
    if (result.status == Balboa::CommandStatus::Accepted) {
      Serial.printf("[smart] set temp -> %.0f\n", setTemp);
      lastError_[0] = '\0';
    } else {
      snprintf(lastError_, sizeof(lastError_), "couldn't set %.0f°F: %s", setTemp, result.message);
      Serial.printf("[smart] %s\n", lastError_);
    }
    controller_.commandSent(setTemp, now);  // either way, wait before trying again
  }
  if (controller_.dirty()) {
    store_.saveSmart(controller_);
  }

  const LocalSlot slot = localSlot(now, config_.tou.holidays);
  const bool intent = controller_.lastIntentAt() != seenIntentAt_;
  seenIntentAt_ = controller_.lastIntentAt();
  usage_.observe(now, slot.date, slot.weekend, slot.slot, tick.inUse, intent);
  if (usage_.dirty()) {
    store_.saveUsage(usage_);
  }
}

bool SmartHeat::handles(const char *action) {
  return strcmp(action, "soak") == 0 || strcmp(action, "cancelSoak") == 0 || strcmp(action, "resumeSmart") == 0;
}

Balboa::CommandResult SmartHeat::handleCommand(JsonVariantConst command) {
  const char *action = command["action"] | "";
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  std::lock_guard<std::mutex> lock(mutex_);
  const ControllerTick tick = makeTick(now);
  if (strcmp(action, "cancelSoak") == 0) {
    controller_.cancelSoak(tick);
    lastTickMs_ = 0;  // act on it right away
    return {Balboa::CommandStatus::Accepted, "soak cancelled"};
  }
  if (strcmp(action, "resumeSmart") == 0) {
    controller_.endManual();
    lastTickMs_ = 0;
    return {Balboa::CommandStatus::Accepted, "smart heating resumed"};
  }
  if (!spa_.hasFreshState()) {
    return {Balboa::CommandStatus::SpaStateUnavailable, "waiting for the spa"};
  }
  if (!spa_.state().tempRangeHigh) {
    return {Balboa::CommandStatus::InvalidValue, "switch the spa to the high temp range first"};
  }
  const float target = command["target"] | spa_.state().setTemp;
  const uint32_t readyAt = command["readyAt"] | 0;
  const float hours = command["hours"] | 2.0f;
  if (const char *error = controller_.scheduleSoak(target, readyAt, hours, tick, makeContext())) {
    snprintf(message_, sizeof(message_), "%s", error);
    return {Balboa::CommandStatus::InvalidValue, message_};
  }
  lastTickMs_ = 0;
  return {Balboa::CommandStatus::Accepted, "soak scheduled"};
}

void SmartHeat::writeStateJson(JsonObject out, uint32_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  controller_.writeStateJson(out, now);
  if (lastError_[0]) {
    out["error"] = lastError_;
  }
}

void SmartHeat::writePlanJson(JsonObject out) {
  std::lock_guard<std::mutex> lock(mutex_);
  controller_.writePlanJson(out);
  usage_.toJson(out["usage"].to<JsonObject>());
  out["tickMaxMs"] = tickMaxMs_;
}

ControllerTick SmartHeat::makeTick(uint32_t now) const {
  const Balboa::SpaState &state = spa_.state();
  ControllerTick tick;
  tick.now = now;
  tick.setTemp = state.setTemp;
  tick.waterTemp = estimateWaterTemp(now);
  tick.inUse = inUse_;
  tick.heating = state.heating;
  return tick;
}

PlanContext SmartHeat::makeContext() const {
  PlanContext context;
  context.tou = config_.tou;
  context.rates = config_.rates;
  context.heaterKw = config_.power.heaterKw;
  context.heaterRate = thermal_.heaterRate();
  context.lossCoeff = thermal_.lossCoeff();
  context.minTemp = config_.smartMinTemp;
  context.autoEnabled = config_.smartAuto;
  context.readyTemp = config_.smartReadyTemp;
  context.lowRange = !spa_.state().tempRangeHigh;
  const WeatherClient &weather = weather_;
  context.outsideAt = [&weather](uint32_t at) { return weather.outsideAt(at); };
  const UsageModel &usage = usage_;
  const HolidaySet holidays = config_.tou.holidays;
  context.readyWithinHours = [&usage, holidays](uint32_t at) {
    const LocalSlot local = localSlot(at, holidays);
    return usage.readyWithinHours(local.weekend, local.slot);
  };
  return context;
}

// The spa only reports water temperature after its pump runs, so between readings the last one is cooled
// by the learned model for the time since. While heating the reading is live anyway.
float SmartHeat::estimateWaterTemp(uint32_t now) const {
  const Balboa::SpaState &state = spa_.state();
  if (!isnan(state.waterTemp)) {
    return state.waterTemp;
  }
  const float last = spa_.currentWaterTemp();
  const uint32_t at = spa_.lastWaterTempAt();
  if (isnan(last) || !at || now <= at || state.heating) {
    return last;
  }
  const float outside = weather_.outsideAt(now);
  const float air = isnan(outside) ? ThermalModel::FallbackOutsideF : outside;
  const float hours = static_cast<float>(now - at) / 3600.0f;
  return air + (last - air) * expf(-thermal_.lossCoeff() * hours);
}
