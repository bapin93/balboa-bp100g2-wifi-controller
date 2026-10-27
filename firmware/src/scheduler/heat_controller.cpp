#include "scheduler/heat_controller.h"

#include <algorithm>

namespace {

constexpr uint32_t ClockValidAfter = 1700000000;
constexpr size_t MaxPlanSteps = 36 * 3600 / HeatController::StepSec;
constexpr float MaxTemp = 104;

void writeTime(JsonObject out, const char *key, uint32_t value) {
  if (value) {
    out[key] = value;
  } else {
    out[key] = nullptr;
  }
}

}  // namespace

float HeatController::tick(const ControllerTick &tick, const PlanContext &context) {
  if (tick.now < ClockValidAfter || isnan(tick.setTemp)) {
    return NAN;
  }
  const uint32_t now = tick.now;
  lowRange_ = context.lowRange;
  if (lowRange_) {
    // Forget the set temp so neither switch of range (each keeps its own set temp) looks like a change by hand.
    lastSeenSet_ = NAN;
    pendingSet_ = NAN;
    restoreTemp_ = NAN;
    if (soak_.active) {
      soak_.active = false;
      dirty_ = true;
    }
    plan_ = PlanResult();
    replanNeeded_ = true;
    return NAN;
  }
  if (context.readyTemp != comfort_) {
    comfort_ = context.readyTemp;
    replanNeeded_ = true;
  }
  if (context.autoEnabled != autoEnabled_) {
    autoEnabled_ = context.autoEnabled;
    replanNeeded_ = true;
    // Turning Auto off puts the comfort temp back rather than leaving a standby setting behind.
    if (!autoEnabled_ && !soak_.active && !manual_.active) {
      plan_ = PlanResult();
      startRestore(comfort_, now);
    }
  }
  noteSetTemp(tick);
  if (tick.inUse) {
    if (manual_.active) {
      manual_.lastUse = now;
    }
    if (soak_.active && now >= soak_.readyAt) {
      soak_.lastUse = now;
    }
  }
  if (soak_.active && !soakArrived_ && now >= soak_.readyAt) {
    soakArrived_ = true;
    lastIntentAt_ = now;
  }
  if (manual_.active && !tick.inUse && now >= manualUntil()) {
    manual_.active = false;
    replanNeeded_ = true;
    dirty_ = true;
  }
  readyInHours_ = isnan(tick.waterTemp) ? NAN
                                        : hoursToReach(tick.waterTemp, comfort_, context.heaterRate,
                                                       context.lossCoeff, context.outsideAt(now));
  if (manual_.active) {
    return NAN;
  }

  float desired = NAN;
  bool planned = false;  // from the plan, rather than something the owner asked for
  if (!isnan(restoreTemp_)) {
    if (now >= restoreUntil_ || roundf(tick.setTemp) == restoreTemp_) {
      restoreTemp_ = NAN;
    } else {
      desired = restoreTemp_;
    }
  } else if (soak_.active) {
    if (now >= soakEnd() && !tick.inUse) {
      endSoak(now);
      desired = restoreTemp_;  // NAN with Auto on: it takes over below
    } else if (now >= soak_.readyAt || soak_.asap) {
      // As soon as possible means straight through. The plan is only kept for the app's chart.
      if (replanNeeded_ || !plan_.ok || now >= planStart_ + StepSec) {
        replan(tick, context);
      }
      desired = soak_.target;
    } else {
      if (replanNeeded_ || !plan_.ok || now >= planStart_ + StepSec) {
        replan(tick, context);
      }
      // Without a water temp there's no plan; heating now is the safe way to still be ready.
      desired = plan_.ok ? setpointFromPlan(now, context, tick.heating) : soak_.target;
      planned = plan_.ok;
    }
  }
  if (isnan(desired) && autoEnabled_ && !soak_.active && isnan(restoreTemp_)) {
    if (replanNeeded_ || !plan_.ok || now >= planStart_ + StepSec) {
      replan(tick, context);
    }
    desired = plan_.ok ? setpointFromPlan(now, context, tick.heating) : comfort_;
    planned = plan_.ok;
  }
  if (isnan(desired)) {
    return NAN;
  }
  desired = roundf(desired);
  if (desired >= roundf(tick.setTemp)) {
    lowerNow_ = false;
  }
  if (desired == roundf(tick.setTemp)) {
    pendingSet_ = NAN;
    return NAN;
  }
  if (tick.inUse && desired < tick.setTemp && !lowerNow_) {
    return NAN;  // never cool the water on someone, unless they just ended their soak
  }
  if (planned && desired < tick.setTemp && tick.heating && !lowerNow_) {
    if (!lowerHeldSince_) {
      lowerHeldSince_ = now;
    }
    if (now - lowerHeldSince_ < LetHeaterFinishSec) {
      return NAN;  // let this heater run finish rather than cut it short
    }
  } else {
    lowerHeldSince_ = 0;
  }
  if (desired == pendingSet_ && now - pendingAt_ < CommandSettleSec) {
    return NAN;  // sent; waiting for the spa to show it
  }
  return desired;
}

void HeatController::commandSent(float setTemp, uint32_t now) {
  lowerNow_ = false;
  pendingSet_ = setTemp;
  pendingAt_ = now;
}

const char *HeatController::scheduleSoak(float target, uint32_t readyAt, float hours, const ControllerTick &tick,
                                         const PlanContext &context) {
  if (tick.now < ClockValidAfter) {
    return "the clock isn't set yet";
  }
  if (!(target >= 80 && target <= MaxTemp)) {
    return "target must be 80-104°F";
  }
  if (!(hours >= 0.5f && hours <= 6)) {
    return "soak length must be 30 minutes to 6 hours";
  }
  if (readyAt && (readyAt < tick.now || readyAt > tick.now + MaxAheadSec)) {
    return "ready time must be within the next 30 hours";
  }
  const bool asap = readyAt == 0;
  if (asap) {
    if (isnan(tick.waterTemp)) {
      readyAt = tick.now;  // no reading to estimate from; the soak just starts now
    } else {
      PlanInput in;
      in.start = tick.now;
      in.stepSec = StepSec;
      in.waterTemp = tick.waterTemp;
      in.heaterKw = context.heaterKw;
      in.heaterRate = context.heaterRate;
      in.lossCoeff = context.lossCoeff;
      in.steps = buildPlanSteps(tick.now, StepSec, MaxPlanSteps, context.tou, context.rates, target, context.outsideAt);
      readyAt = earliestReady(in, target);
      if (!readyAt) {
        return "the heater can't reach that temperature in this weather";
      }
    }
  }
  // Keep the original temp to go back to when one soak replaces another.
  const float restore = soak_.active ? soak_.restoreTemp : manual_.active ? manual_.temp
                        : autoEnabled_ ? comfort_ : roundf(tick.setTemp);
  soak_ = Soak();
  soak_.active = true;
  soak_.target = roundf(target);
  soak_.readyAt = readyAt;
  soak_.hours = hours;
  soak_.asap = asap;
  soak_.restoreTemp = isnan(restore) ? roundf(tick.setTemp) : restore;
  soakArrived_ = false;  // counted as intent once its ready time comes
  lowerNow_ = false;
  manual_.active = false;
  restoreTemp_ = NAN;
  replanNeeded_ = true;
  dirty_ = true;
  return nullptr;
}

// Ending a soak means the owner is done, even with the light or jets still on, so it drops right away.
void HeatController::cancelSoak(const ControllerTick &tick) {
  if (soak_.active) {
    endSoak(tick.now);
  }
}

void HeatController::endManual() {
  if (manual_.active) {
    manual_.active = false;
    replanNeeded_ = true;
    dirty_ = true;
  }
}

void HeatController::endSoak(uint32_t now) {
  soak_.active = false;
  plan_ = PlanResult();
  replanNeeded_ = true;
  dirty_ = true;
  lowerNow_ = true;  // the heater was only keeping the soak hot
  if (!autoEnabled_) {
    startRestore(soak_.restoreTemp, now);
  }
}

void HeatController::noteSetTemp(const ControllerTick &tick) {
  const float set = roundf(tick.setTemp);
  if (isnan(lastSeenSet_)) {
    lastSeenSet_ = set;  // first reading after boot; nothing to compare yet
    return;
  }
  if (set == lastSeenSet_) {
    return;
  }
  const float before = lastSeenSet_;
  lastSeenSet_ = set;
  if (set == pendingSet_ && tick.now - pendingAt_ < CommandSettleSec) {
    pendingSet_ = NAN;  // ours
    return;
  }
  if (set > before) {
    lastIntentAt_ = tick.now;
  }
  dirty_ = true;
  if (!soak_.active && !autoEnabled_) {
    return;  // nothing to take over from
  }
  soak_.active = false;
  plan_ = PlanResult();
  restoreTemp_ = NAN;
  manual_ = Manual();
  manual_.active = true;
  manual_.since = tick.now;
  manual_.temp = set;
}

uint32_t HeatController::soakEnd() const {
  const uint32_t scheduled = soak_.readyAt + static_cast<uint32_t>(soak_.hours * 3600.0f);
  return soak_.lastUse ? std::max(scheduled, soak_.lastUse + SoakAfterUseSec) : scheduled;
}

uint32_t HeatController::manualUntil() const {
  return manual_.lastUse ? manual_.lastUse + ManualAfterUseSec : manual_.since + ManualMaxSec;
}

void HeatController::replan(const ControllerTick &tick, const PlanContext &context) {
  replanNeeded_ = false;
  planStart_ = tick.now - tick.now % StepSec;
  if (isnan(tick.waterTemp)) {
    plan_ = PlanResult();
    return;
  }
  const float comfort = std::min(MaxTemp, std::max(context.minTemp, comfort_));
  planTarget_ = soak_.active ? soak_.target : comfort;
  size_t steps = MaxPlanSteps;
  if (!autoEnabled_) {
    const uint32_t end = soakEnd() + StepSec;
    steps = std::min(MaxPlanSteps, static_cast<size_t>((end - planStart_ + StepSec - 1) / StepSec));
  }
  PlanInput in;
  in.start = planStart_;
  in.stepSec = StepSec;
  in.waterTemp = tick.waterTemp;
  in.heaterKw = context.heaterKw;
  in.heaterRate = context.heaterRate;
  in.lossCoeff = context.lossCoeff;
  in.steps = buildPlanSteps(planStart_, StepSec, steps, context.tou, context.rates, planTarget_, context.outsideAt);
  for (size_t s = 0; s < in.steps.size(); ++s) {
    PlanStep &step = in.steps[s];
    float need = context.minTemp < planTarget_ ? context.minTemp : NAN;
    if (autoEnabled_) {
      const uint32_t end = planStart_ + static_cast<uint32_t>(s + 1) * StepSec;
      const float wait = context.readyWithinHours(end);
      if (!isnan(wait)) {
        const float ready = readinessTemp(comfort, wait, context.heaterRate, context.lossCoeff, step.outside);
        need = std::max(isnan(need) ? context.minTemp : need, std::min(comfort, ready));
      }
    }
    step.minTemp = need;
  }
  if (soak_.active) {
    // As soon as possible heats straight through, so the plan wants the target from the start.
    const uint32_t from = soak_.asap ? planStart_ : soak_.readyAt;
    addSoak(in, soak_.target, from, static_cast<float>(soakEnd() - from) / 3600.0f);
  }
  plan_ = HeatPlanner::plan(in);
  holdPlan_ = PlanResult();
  fairPlan_ = PlanResult();
  if (autoEnabled_ && !soak_.active) {
    PlanInput hold = in;
    for (PlanStep &step : hold.steps) {
      step.minTemp = comfort;
    }
    holdPlan_ = HeatPlanner::plan(hold);
    PlanInput fair = in;
    fair.steps.back().minTemp = comfort;
    fairPlan_ = HeatPlanner::plan(fair);
  }
  planMin_.clear();
  planPrice_.clear();
  for (const PlanStep &step : in.steps) {
    planMin_.push_back(step.minTemp);
    planPrice_.push_back(step.price);
  }
}

size_t HeatController::stepAt(uint32_t now) const {
  return now < planStart_ ? 0 : (now - planStart_) / StepSec;
}

// Heating: the highest temp in this run of heated steps, so the set temp stays put for the whole run. Runs
// separated by a pause of MergeGapSteps or less count as one, so the heater doesn't stop and restart for it.
// Coasting: a little under the lowest temp before heating resumes, so the spa only steps in if the
// water cools faster than planned.
float HeatController::setpointFromPlan(uint32_t now, const PlanContext &context, bool heaterOn) const {
  const size_t count = plan_.heat.size();
  size_t s = stepAt(now);
  if (s >= count) {
    return planTarget_;
  }
  // Index of the step where heating resumes after a short pause starting at `from`, or `from` if it doesn't.
  const auto resumesSoon = [&](size_t from) {
    size_t next = from;
    while (next < count && plan_.heat[next] == 0 && next - from < MergeGapSteps) {
      ++next;
    }
    return next < count && plan_.heat[next] > 0 ? next : from;
  };
  if (plan_.heat[s] == 0 && heaterOn) {
    s = resumesSoon(s);  // still running from the last stretch, and more is coming: keep going
  }
  if (plan_.heat[s] > 0) {
    float high = plan_.temp[s];
    for (;;) {
      for (; s < count && plan_.heat[s] > 0; ++s) {
        high = std::max(high, plan_.temp[s]);
      }
      const size_t next = resumesSoon(s);
      if (next == s) {
        break;
      }
      s = next;
    }
    return std::min(planTarget_, ceilf(high - 0.05f));
  }
  float low = plan_.temp[s];
  for (; s < count && plan_.heat[s] == 0; ++s) {
    low = std::min(low, plan_.temp[s]);
  }
  return std::max(context.minTemp, floorf(low) - CoastMarginF);
}

void HeatController::startRestore(float temp, uint32_t now) {
  if (isnan(temp)) {
    return;
  }
  restoreTemp_ = roundf(temp);
  restoreUntil_ = now + RestoreTrySec;
}

void HeatController::writeNextHeat(JsonObject out, uint32_t now) const {
  const size_t s = stepAt(now);
  const bool planned = plan_.ok && s < plan_.heat.size();
  out["phase"] = !planned || plan_.heat[s] > 0 ? "heating" : "waiting";
  uint32_t nextHeat = 0;
  for (size_t i = s; planned && i < plan_.heat.size(); ++i) {
    if (plan_.heat[i] > 0) {
      nextHeat = planStart_ + static_cast<uint32_t>(i) * StepSec;  // fixed, so the state only changes when the plan does
      break;
    }
  }
  writeTime(out, "nextHeatAt", nextHeat);
}

void HeatController::writeStateJson(JsonObject out, uint32_t now) const {
  out["mode"] = lowRange_ ? "lowRange" : manual_.active ? "manual" : soak_.active ? "soak" : autoEnabled_ ? "auto" : "idle";
  if (!isnan(comfort_)) {
    out["comfortTemp"] = comfort_;
  }
  if (!isnan(readyInHours_)) {
    out["readyInMin"] = isinf(readyInHours_) ? -1 : static_cast<int>(roundf(readyInHours_ * 60));
  }
  if (manual_.active) {
    JsonObject manual = out["manual"].to<JsonObject>();
    manual["since"] = manual_.since;
    manual["temp"] = manual_.temp;
    manual["until"] = manualUntil();
  }
  if (autoEnabled_ && !lowRange_ && !soak_.active && !manual_.active) {
    writeNextHeat(out["auto"].to<JsonObject>(), now);
  }
  if (!soak_.active) {
    return;
  }
  JsonObject soak = out["soak"].to<JsonObject>();
  soak["target"] = soak_.target;
  soak["readyAt"] = soak_.readyAt;
  soak["hours"] = soak_.hours;
  soak["asap"] = soak_.asap;
  soak["endsAt"] = soakEnd();
  if (now >= soak_.readyAt) {
    soak["phase"] = "ready";
    return;
  }
  if (soak_.asap) {
    soak["phase"] = "heating";
    return;
  }
  writeNextHeat(soak, now);
  if (plan_.ok) {
    soak["cost"] = roundf(plan_.cost * 100) / 100;
    soak["shortfall"] = roundf(plan_.shortfall * 10) / 10;
  }
}

void HeatController::writePlanJson(JsonObject out) const {
  if (!plan_.ok || lowRange_ || manual_.active || (!soak_.active && !autoEnabled_)) {
    out["plan"] = nullptr;
    return;
  }
  JsonObject plan = out["plan"].to<JsonObject>();
  plan["start"] = planStart_;
  plan["stepSec"] = StepSec;
  plan["target"] = planTarget_;
  plan["kwh"] = roundf(plan_.kwh * 100) / 100;
  plan["cost"] = roundf(plan_.cost * 1000) / 1000;
  if (holdPlan_.ok && fairPlan_.ok) {
    // Both sides start from the water now and end at the comfort temp.
    JsonObject compare = plan["compare"].to<JsonObject>();
    compare["holdCost"] = roundf(holdPlan_.cost * 1000) / 1000;
    compare["holdKwh"] = roundf(holdPlan_.kwh * 100) / 100;
    compare["smartCost"] = roundf(fairPlan_.cost * 1000) / 1000;
    compare["smartKwh"] = roundf(fairPlan_.kwh * 100) / 100;
  }
  JsonArray steps = plan["steps"].to<JsonArray>();
  for (size_t s = 0; s < plan_.temp.size(); ++s) {
    JsonObject step = steps.add<JsonObject>();
    step["heat"] = roundf(plan_.heat[s] * 100) / 100;
    step["temp"] = roundf(plan_.temp[s] * 10) / 10;
    step["price"] = planPrice_[s];
    if (s < planMin_.size() && !isnan(planMin_[s])) {
      step["min"] = roundf(planMin_[s] * 10) / 10;
    }
  }
}

void HeatController::toJson(JsonObject out) const {
  if (soak_.active) {
    JsonObject soak = out["soak"].to<JsonObject>();
    soak["target"] = soak_.target;
    soak["readyAt"] = soak_.readyAt;
    soak["hours"] = soak_.hours;
    soak["asap"] = soak_.asap;
    if (!isnan(soak_.restoreTemp)) {
      soak["restore"] = soak_.restoreTemp;
    }
    soak["lastUse"] = soak_.lastUse;
  }
  if (manual_.active) {
    JsonObject manual = out["manual"].to<JsonObject>();
    manual["since"] = manual_.since;
    manual["temp"] = manual_.temp;
    manual["lastUse"] = manual_.lastUse;
  }
}

void HeatController::fromJson(JsonVariantConst in) {
  soak_ = Soak();
  if (in["soak"].is<JsonObjectConst>()) {
    JsonVariantConst soak = in["soak"];
    soak_.target = soak["target"] | NAN;
    soak_.readyAt = soak["readyAt"] | 0;
    soak_.hours = soak["hours"] | 2.0f;
    soak_.asap = soak["asap"] | false;
    soak_.restoreTemp = soak["restore"] | NAN;
    soak_.lastUse = soak["lastUse"] | 0;
    soak_.active = !isnan(soak_.target) && soak_.readyAt;
  }
  manual_ = Manual();
  if (in["manual"].is<JsonObjectConst>()) {
    JsonVariantConst manual = in["manual"];
    manual_.since = manual["since"] | 0;
    manual_.temp = manual["temp"] | NAN;
    manual_.lastUse = manual["lastUse"] | 0;
    manual_.active = manual_.since != 0;
  }
  replanNeeded_ = true;
}
