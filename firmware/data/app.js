const state = {
  data: null,
  config: null,
  ws: null,
  modalTemp: null,
  toastTimer: null,
  filterSheet: null,
  capturePollTimer: null,
  commandModalOpen: false,
  commandModalStatus: "idle",
  commandWait: null,
  commandResolve: null,
  commandSuccessTimer: null,
  pumpSeq: null,
  spaInfoSeq: null,
  faultLogSeq: null,
  faultLogRequested: false,
  runtimeDays: null,
  runtimeHours: null,
  weather: null,
  runtimeLoadedAt: 0,
  faultLog: null,
  maintenance: null,
  maintClockOffset: 0,
  maintLoadedAt: 0,
  maintSheet: null,
  maintAlerts: { tasks: false, filters: false, water: false },
  historyExpanded: { task: false, filter: false, water: false },
  water: null,
  waterSheet: null,
  tempRangePending: null,
};

const ReminderText = {
  0x04: "Time to clean the filter",
  0x09: "Check the sanitizer",
  0x0a: "Check the pH",
  0x1e: "The spa reported a fault — see Diagnostics",
};

// Fault codes from the Balboa protocol notes (github.com/ccutrer/balboa_worldwide_app/wiki).
const FaultText = {
  15: "Sensors are out of sync",
  16: "Low water flow",
  17: "Water flow has failed",
  18: "Settings have been reset",
  19: "Priming mode",
  20: "The clock has failed",
  21: "Settings have been reset",
  22: "Program memory failure",
  26: "Sensors are out of sync — call for service",
  27: "The heater is dry",
  28: "The heater may be dry",
  29: "The water is too hot",
  30: "The heater is too hot",
  31: "Sensor A fault",
  32: "Sensor B fault",
  34: "A pump may be stuck on",
  35: "Hot fault",
  36: "The GFCI test failed",
  37: "Standby (hold) mode",
};

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => Array.from(document.querySelectorAll(selector));

// "just now", "40 min ago", "3 h ago"; "" when the time is unknown.
function ageText(epochSeconds) {
  if (!epochSeconds) return "";
  const minutes = Math.max(0, Math.round((Date.now() / 1000 - epochSeconds) / 60));
  if (minutes < 1) return "just now";
  if (minutes < 60) return `${minutes} min ago`;
  if (minutes < 48 * 60) return `${Math.round(minutes / 60)} h ago`;
  return `${Math.round(minutes / 1440)} days ago`;
}

// The spa reports water temperature only after the pump runs; between readings show the last one, marked.
function waterReading(data) {
  if (Number.isFinite(data.waterTemp)) return { temp: data.waterTemp, stale: false, age: "" };
  if (Number.isFinite(data.lastWaterTemp)) return { temp: data.lastWaterTemp, stale: true, age: ageText(data.lastWaterTempAt) };
  return { temp: NaN, stale: false, age: "" };
}

function tempLabel(value) {
  return Number.isFinite(value) ? `${Math.round(value)}°F` : "--";
}

function jetLabel(active, speed = 0) {
  if (!active) return "Off";
  if (speed === 1) return "Low";
  if (speed === 2) return "High";
  return "On";
}

function activeFilterLabel(cycle) {
  if (cycle === 1) return "Filter 1 Running";
  if (cycle === 2) return "Filter 2 Running";
  return "No Filter Running";
}

function heaterLabel(data) {
  if (data.heating) return "Heater On";
  if (data.heatPending) return "Heat Waiting";
  return "Heater Off";
}

function runtimeLabel(hour, minute = 0) {
  return `${Math.max(0, Math.round(hour || 0))}:${String(Math.max(0, Math.min(59, Math.round(minute || 0)))).padStart(2, "0")}`;
}

function toClock12(hour24) {
  const normalized = ((Math.round(hour24) % 24) + 24) % 24;
  const hour = normalized % 12 || 12;
  const period = normalized < 12 ? "AM" : "PM";
  return { hour, period };
}

function toHour24(hour12, period) {
  const normalized = Math.max(1, Math.min(12, Math.round(hour12)));
  if (period === "AM") {
    return normalized === 12 ? 0 : normalized;
  }
  return normalized === 12 ? 12 : normalized + 12;
}

function clock12Label(hour24, minute = 0) {
  const clock = toClock12(hour24);
  const normalizedMinute = Number.isFinite(minute) ? Math.max(0, Math.min(59, Math.round(minute))) : 0;
  return `${clock.hour}:${String(normalizedMinute).padStart(2, "0")} ${clock.period}`;
}

function setValue(id, text, muted = false) {
  const el = $(`#${id}`);
  el.textContent = text;
  el.classList.toggle("muted", muted);
}

function minutesLabel(totalMinutes) {
  const hours = Math.floor(totalMinutes / 60);
  const minutes = totalMinutes % 60;
  return hours ? `${hours}h ${String(minutes).padStart(2, "0")}m` : `${minutes}m`;
}

// "2:00–5:00 AM", or "10:00 PM–1:00 AM" when it crosses AM/PM.
function rangeLabel(startMinute, lengthMinutes) {
  if (lengthMinutes >= 24 * 60) return "All day";
  const end = (startMinute + lengthMinutes) % (24 * 60);
  const from = clock12Label(Math.floor(startMinute / 60), startMinute % 60);
  const to = clock12Label(Math.floor(end / 60), end % 60);
  const [fromTime, fromPeriod] = from.split(" ");
  return fromPeriod === to.split(" ")[1] ? `${fromTime}–${to}` : `${from}–${to}`;
}

function filterCycles(filters) {
  return [1, 2]
    .map((n) => ({
      n,
      enabled: n === 1 || !!filters.cycle2Enabled,
      start: (filters[`cycle${n}Start`] ?? 0) * 60 + (filters[`cycle${n}StartMinute`] ?? 0),
      length: (filters[`cycle${n}Duration`] ?? 0) * 60 + (filters[`cycle${n}DurationMinute`] ?? 0),
    }))
    .map((cycle) => ({ ...cycle, enabled: cycle.enabled && cycle.length > 0 }));
}

function spaMinutes(spaTime) {
  const match = /^(\d{1,2}):(\d{2})$/.exec(spaTime || "");
  return match ? Number(match[1]) * 60 + Number(match[2]) : null;
}

function renderOverview(data, caps) {
  const water = waterReading(data);
  setValue("ovWater", tempLabel(water.temp));
  $("#ovWater").classList.toggle("stale", water.stale);
  $("#ovWaterLabel").textContent = water.stale ? "Water · last reading" : "Water";
  setValue("ovSet", tempLabel(data.setTemp));
  const toGo = Math.round((data.setTemp ?? 0) - (data.waterTemp ?? 0));
  const heroStatus = $("#ovHeatStatus");
  // The spa only knows the water temperature after the pump has run past its sensor.
  const waterKnown = Number.isFinite(data.waterTemp);
  heroStatus.textContent = !data.valid ? "Waiting for the spa"
    : data.hold ? "Paused for hold"
    : !waterKnown ? [data.heating ? "Heating" : "", water.stale && water.age ? `last reading ${water.age}` : "no reading yet"]
      .filter(Boolean).join(" · ").replace(/^./, (c) => c.toUpperCase())
    : data.heating ? `Heating · ${toGo}°F to go`
    : data.heatPending ? "Heat waiting"
    : toGo > 1 ? `${toGo}°F below set point` : "At temperature";
  heroStatus.classList.toggle("active", !!data.heating);
  heroStatus.classList.toggle("pending", !data.heating && !!data.heatPending);

  setValue("ovHeater", data.heating ? "Heating" : data.heatPending ? "Waiting" : "Off", !data.heating && !data.heatPending);
  setValue("ovJet1", speedLabel(data.jet1Speed ?? 0, pumpSpeedCount(1, caps)), !data.jet1Speed);
  $("#ovJet2Row").hidden = !caps.jet2;
  setValue("ovJet2", speedLabel(data.jet2Speed ?? 0, pumpSpeedCount(2, caps)), !data.jet2Speed);
  setValue("ovLight", data.light ? "On" : "Off", !data.light);
  $("#ovHoldRow").hidden = !data.hold;
  setValue("ovHold", `${data.holdMinutes} min left`);
  setValue("ovHeatMode", ["Ready", "Rest", "Ready-in-Rest"][data.heatMode] || "Unknown");
  const range = data.setTempRange;
  setValue("ovTempRange", `${data.tempRangeHigh ? "High" : "Low"}${range ? ` (${range.min}–${range.max}°F)` : ""}`);

  const filters = data.filterCycles || {};
  const cycles = filterCycles(filters);
  cycles.forEach((cycle) => {
    setValue(`ovFilter${cycle.n}`, cycle.enabled ? rangeLabel(cycle.start, cycle.length) : "Off", !cycle.enabled);
  });
  const now = spaMinutes(data.spaTime);
  let next = "No filters scheduled";
  if (now !== null) {
    const running = cycles.find((cycle) => cycle.enabled && (now - cycle.start + 1440) % 1440 < cycle.length);
    if (running) {
      const end = (running.start + running.length) % 1440;
      next = `Filter ${running.n} running · ends ${clock12Label(Math.floor(end / 60), end % 60)}`;
    } else {
      const upcoming = cycles
        .filter((cycle) => cycle.enabled)
        .map((cycle) => ({ ...cycle, wait: (cycle.start - now + 1440) % 1440 }))
        .sort((a, b) => a.wait - b.wait)[0];
      if (upcoming) next = `Filter ${upcoming.n} in ${minutesLabel(upcoming.wait)}`;
    }
  }
  setValue("ovFilterNext", next);

  setValue("ovLink", data.valid ? "Live" : "Waiting for the spa", !data.valid);
  setValue("ovSpaClock", data.valid ? spaClockLabel(data.spaTime) : "--");
  if (now !== null && data.valid) {
    const phone = new Date();
    const drift = ((now - (phone.getHours() * 60 + phone.getMinutes()) + 720) % 1440 + 1440) % 1440 - 720;
    setValue("ovClockDrift", Math.abs(drift) <= 1 ? "In sync" : `${Math.abs(drift)} min ${drift > 0 ? "fast" : "slow"} · Sync in Settings`, Math.abs(drift) <= 1);
  } else {
    setValue("ovClockDrift", "--", true);
  }
  if (!state.faultLog?.read) {
    setValue("ovLastFault", "Not read yet", true);
  } else if (!state.faultLog.entries.length) {
    setValue("ovLastFault", "None recorded", true);
  } else {
    const latest = state.faultLog.entries[0];
    setValue("ovLastFault", `${FaultText[latest.code] || `Code ${latest.code}`} · ${faultDaysText(latest.daysAgo).toLowerCase()}`);
  }

  renderOverviewToday();
  const overviewVisible = $("#stats").classList.contains("active") && $("#statsOverview").classList.contains("active");
  if (overviewVisible && Date.now() - state.runtimeLoadedAt > 60000) {
    state.runtimeLoadedAt = Date.now();  // throttle; loadRuntime sets it again on success
    loadRuntime().catch((error) => console.warn("[bp100g2] runtime failed", error));
  }
}

function renderOverviewToday() {
  const days = state.runtimeDays;
  if (!days) {
    ["ovTodayHeater", "ovTodayJets", "ovTodayFilter", "ovHeaterAvg"].forEach((id) => setValue(id, "--", true));
    return;
  }
  const now = new Date();
  const todayDate = now.getFullYear() * 10000 + (now.getMonth() + 1) * 100 + now.getDate();
  const today = days.find((day) => day.date === todayDate);
  const mins = (seconds) => Math.round((seconds || 0) / 60);
  setValue("ovTodayHeater", today ? minutesLabel(mins(today.heatingSec)) : "0m", !today?.heatingSec);
  const jetsTotal = today ? mins(today.pumpLowSec) + mins(today.pumpHighSec) : 0;
  const high = today ? mins(today.pumpHighSec) : 0;
  setValue("ovTodayJets", `${minutesLabel(jetsTotal)}${high ? ` · ${minutesLabel(high)} high` : ""}`, !jetsTotal);
  setValue("ovTodayFilter", today ? minutesLabel(mins(today.filterSec)) : "0m", !today?.filterSec);
  const past = days.filter((day) => day.date !== todayDate).slice(-7);
  if (past.length) {
    const average = past.reduce((sum, day) => sum + mins(day.heatingSec), 0) / past.length;
    setValue("ovHeaterAvg", `${minutesLabel(Math.round(average))} / day`);
  } else {
    setValue("ovHeaterAvg", "Not enough history", true);
  }
  $("#ovTodayCostRow").hidden = !ratesSet() || !today;
  if (ratesSet() && today) setValue("ovTodayCost", costText(dayEnergy(today)));
}

function filterSummary(values) {
  if (values.enabled === false) {
    return "Enabled: Off";
  }
  return `Start: ${clock12Label(values.startHour, values.startMinute)}\nRuntime: ${runtimeLabel(values.durationHour, values.durationMinute)}`;
}

function setConnection(text, ok = false) {
  const el = $("#connection");
  const pill = $(".conn-pill");
  el.textContent = text;
  pill?.classList.toggle("ok", ok);
}

function showToast(text) {
  const toast = $("#toast");
  toast.textContent = text;
  toast.classList.add("show");
  if (state.toastTimer) {
    clearTimeout(state.toastTimer);
  }
  state.toastTimer = setTimeout(() => {
    toast.classList.remove("show");
    state.toastTimer = null;
  }, 2600);
}

function field(form, name) {
  return form.elements.namedItem(name);
}

function renderState(data) {
  state.data = data;
  const water = waterReading(data);
  $("#waterTemp").textContent = tempLabel(water.temp);
  $("#waterTemp").classList.toggle("stale", water.stale);
  $("#waterLabel").textContent = water.stale ? `water · ${water.age || "earlier"}` : "water";
  $("#setTemp").textContent = tempLabel(data.setTemp);
  $("#heatingRing").classList.toggle("active", !!data.heating);
  $("#heatingRing").classList.toggle("pending", !data.heating && !!data.heatPending);
  $("#heaterStatus").textContent = heaterLabel(data);
  $("#heaterStatus").classList.toggle("active", !!data.heating);
  $("#heaterStatus").classList.toggle("pending", !data.heating && !!data.heatPending);
  $("#filterStatus").textContent = activeFilterLabel(data.activeFilterCycle);
  $("#filterStatus").classList.toggle("active", !!data.activeFilterCycle);
  $("#spaClock").textContent = data.valid ? spaClockLabel(data.spaTime) : "--";
  $("#settingsSpaTime").textContent = $("#spaClock").textContent;
  renderTempRange(data);
  $("#captureState").textContent = data.statusCaptureActive ? "Capturing" : "Stopped";
  $("#captureSummary").textContent = data.statusCaptureActive ? "Capturing" : "For troubleshooting";
  $("#captureCount").textContent = `${data.statusCaptureLines ?? 0} lines`;
  $("#startCapture").disabled = !!data.statusCaptureActive;
  $("#stopCapture").disabled = !data.statusCaptureActive;
  renderFilterSettings(data);

  const caps = data.capabilities || {};
  $("#syncTime").hidden = caps.setTime === false;
  $("#equipmentSection").hidden = !caps.pump2Installed;
  $("#pump2PowerRow").hidden = !caps.pump2Installed || !!state.config?.capabilities?.hideJet2;
  $$("[data-action]").forEach((button) => {
    const action = button.dataset.action;
    button.hidden = caps[action] === false;
    const active = action === "light" ? data.light : action === "hold" ? data.hold : false;
    button.classList.toggle("active", !!active);
    button.setAttribute("aria-pressed", active ? "true" : "false");
  });
  $("#holdToggle").textContent = data.hold ? "End Hold" : "Start Hold";
  $("#holdStatus").textContent = data.hold ? `On · ${data.holdMinutes} min left` : "Off";
  renderPumps(data, caps);
  renderOverview(data, caps);
  const runtimeVisible = $("#stats").classList.contains("active") && $("#statsRuntime").classList.contains("active");
  if (runtimeVisible && state.runtimeHours && state.hoursChartLive !== hoursLiveKey(data)) {
    renderHoursChart();  // keep its "now" point current
  }
  const maintRefreshMs = $("#maintenance").classList.contains("active") ? 60000 : 600000;
  if (Date.now() - state.maintLoadedAt > maintRefreshMs) {
    state.maintLoadedAt = Date.now();  // throttle; pumped hours only change slowly
    loadMaintenance().catch((error) => console.warn("[bp100g2] maintenance failed", error));
    loadWater().catch((error) => console.warn("[bp100g2] water log failed", error));
  }
  renderBanners(data);
  renderSmart(data);
  if (state.pumpSeq !== null && data.pumpCommand?.seq > state.pumpSeq && !data.pumpCommand.ok) {
    showToast(data.pumpCommand.message);
  }
  state.pumpSeq = data.pumpCommand?.seq ?? 0;
  if (data.spaInfoSeq !== state.spaInfoSeq) {
    state.spaInfoSeq = data.spaInfoSeq;
    loadSpaInfo().catch((error) => console.warn("[bp100g2] spa info failed", error));
  }
  if (data.faultLogSeq !== state.faultLogSeq) {
    const finished = state.faultLogSeq !== null;
    state.faultLogSeq = data.faultLogSeq;
    if (finished && state.faultLogRequested) {
      loadFaults().catch((error) => showToast(error.message));
    }
  }
  $("#faultLogState").textContent = data.faultLogReading ? "Reading from spa…" : $("#faultLogState").textContent;
  $("#refreshFaults").disabled = !!data.faultLogReading;

  if (data.statusCaptureActive && !state.capturePollTimer) {
    startCapturePolling();
  } else if (!data.statusCaptureActive && state.capturePollTimer) {
    stopCapturePolling();
  }

  updateCommandWait(data);
}

function clampTemp(value) {
  const range = state.data?.setTempRange || { min: 50, max: 104 };
  return Math.max(range.min, Math.min(range.max, Math.round(value)));
}

function pumpSpeedCount(pump, caps) {
  return pump === 1 ? caps.pump1Speeds ?? 2 : caps.pump2Speeds ?? 0;
}

function speedLabel(speed, speeds) {
  if (speed === 0) return "Off";
  if (speeds === 1) return "On";
  return speed === 1 ? "Low" : "High";
}

function renderPumps(data, caps) {
  const pendingPump = data.pumpCommand?.active ? data.pumpCommand.pump : 0;
  $$(".jet-btn").forEach((button) => {
    const pump = Number(button.dataset.pump);
    const speeds = pumpSpeedCount(pump, caps);
    const speed = data[`jet${pump}Speed`] ?? 0;
    button.hidden = speeds === 0;
    button.classList.toggle("active", speed > 0);
    button.classList.toggle("pending", pendingPump === pump);
    button.setAttribute("aria-pressed", speed > 0 ? "true" : "false");
    $(`#jet${pump}ButtonState`).textContent = speedLabel(speed, speeds);
  });
}

function openPumpMenu(pump) {
  const speeds = pumpSpeedCount(pump, state.data?.capabilities || {});
  const current = state.data?.[`jet${pump}Speed`] ?? 0;
  $("#pumpMenuTitle").textContent = `Jet ${pump}`;
  const options = [];
  for (let speed = 0; speed <= speeds; speed += 1) {
    const option = document.createElement("button");
    option.type = "button";
    option.textContent = speedLabel(speed, speeds);
    option.classList.toggle("current", speed === current);
    option.addEventListener("click", () => {
      closePumpMenu();
      if (speed !== current) sendCommand({ action: "setPump", pump, value: speed });
    });
    options.push(option);
  }
  $("#pumpMenuOptions").replaceChildren(...options);
  $("#pumpMenu").classList.remove("hidden");
}

function closePumpMenu() {
  $("#pumpMenu").classList.add("hidden");
}

const LongPressMs = 450;

// Tap: one press, the spa moves to the next speed. Press and hold: pick a speed from a menu.
function setupJetButtons() {
  $$(".jet-btn").forEach((button) => {
    const pump = Number(button.dataset.pump);
    let timer = null;
    let longPressed = false;
    const cancel = () => {
      clearTimeout(timer);
      timer = null;
    };
    button.addEventListener("pointerdown", () => {
      longPressed = false;
      cancel();
      timer = setTimeout(() => {
        longPressed = true;
        timer = null;
        navigator.vibrate?.(12);
        openPumpMenu(pump);
      }, LongPressMs);
    });
    ["pointerup", "pointerleave", "pointercancel"].forEach((type) => button.addEventListener(type, cancel));
    button.addEventListener("contextmenu", (event) => event.preventDefault());
    button.addEventListener("click", () => {
      if (longPressed) {
        longPressed = false;
        return;
      }
      sendCommand({ action: "togglePump", pump });
    });
  });
  $("#pumpMenuCancel").addEventListener("click", closePumpMenu);
  $("#pumpMenu").addEventListener("click", (event) => {
    if (event.target.id === "pumpMenu") closePumpMenu();
  });
}

// ── smart heating: planned soaks and the manual hold ──

// "7:30 PM", or "Tue 7:30 PM" when it isn't today.
function epochTimeLabel(epoch) {
  const when = new Date(epoch * 1000);
  const time = when.toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" });
  return when.toDateString() === new Date().toDateString()
    ? time : `${when.toLocaleDateString(undefined, { weekday: "short" })} ${time}`;
}

// "1h 05m to 102°", or "102° now" once it's there.
function readyInText(minutes, comfort) {
  if (minutes < 0) return `can't reach ${comfort}° in this weather`;
  if (minutes < 5) return `${comfort}° now`;
  return `${hoursMinutes(minutes * 60)} to ${comfort}°`;
}

function renderSmart(data) {
  const smart = data.smart || {};
  const soak = smart.soak;
  let text = "";
  let action = "";
  let quiet = false;
  if (smart.mode === "lowRange") {
    text = "Low range · smart heating is off until you switch back to high";
    quiet = true;
  } else if (smart.mode === "auto" && Number.isFinite(smart.comfortTemp)) {
    const auto = smart.auto || {};
    const ready = Number.isFinite(smart.readyInMin) ? ` · ${readyInText(smart.readyInMin, smart.comfortTemp)}` : "";
    const plan = auto.phase === "heating" ? "Warming up" : auto.nextHeatAt ? `Standby until ${epochTimeLabel(auto.nextHeatAt)}` : "Standby";
    text = `${plan}${ready}`;
    action = smart.readyInMin >= 5 ? "Heat Now" : "";
    quiet = true;
  } else if (smart.mode === "soak" && soak) {
    if (soak.phase === "ready") {
      text = `Soak at ${soak.target}° until ${epochTimeLabel(soak.endsAt)}`;
      action = "End";
    } else if (soak.asap) {
      text = `Heating to ${soak.target}° · ready about ${epochTimeLabel(soak.readyAt)}`;
      action = "Cancel";
    } else {
      const heater = soak.phase === "heating" ? "heating now"
        : soak.nextHeatAt ? `heater from ${epochTimeLabel(soak.nextHeatAt)}` : "";
      text = [`Soak ${soak.target}° at ${epochTimeLabel(soak.readyAt)}`, heater].filter(Boolean).join(" · ");
      action = "Cancel";
    }
  } else if (smart.mode === "manual" && smart.manual) {
    text = `Your ${smart.manual.temp}° holds until ${epochTimeLabel(smart.manual.until)}`;
    action = "Resume";
  }
  if (smart.error) text = text ? `${text} · ${smart.error}` : smart.error;
  $("#smartBanner").classList.toggle("hidden", !text);
  $("#smartBanner").classList.toggle("quiet", quiet);
  $("#smartText").textContent = text;
  $("#smartAction").hidden = !action;
  $("#smartAction").textContent = action;
  $("#smartAction").dataset.action = { manual: "resumeSmart", auto: "heatNow" }[smart.mode] || "cancelSoak";
  $("#soakButton").classList.toggle("active", smart.mode === "soak");
  $("#soakButtonState").textContent = smart.mode === "soak" && soak ? `${soak.target}°` : "";
}

const SoakPreviewDelayMs = 300;

function soakRequest() {
  const target = Math.round(Number($("#soakTarget").value));
  const hours = Number($("#soakHours").value);
  if ($("#soakWhen").value === "asap") return { target, hours, readyAt: 0 };
  const [hour, minute] = ($("#soakTime").value || "19:30").split(":").map(Number);
  const ready = new Date();
  ready.setHours(hour, minute, 0, 0);
  if (ready.getTime() <= Date.now()) ready.setDate(ready.getDate() + 1);  // already past: tomorrow
  return { target, hours, readyAt: Math.floor(ready.getTime() / 1000) };
}

// Contiguous heated steps as [start, end, peak] in Unix time, up to the end of the soak.
function heatWindows(plan, until) {
  const cheapest = Math.min(...plan.steps.map((step) => step.price));
  const windows = [];
  plan.steps.forEach((step, i) => {
    const start = plan.start + i * plan.stepSec;
    if (step.heat <= 0 || start >= until) return;
    const last = windows[windows.length - 1];
    if (last && last.end === start) {
      last.end = start + plan.stepSec;
      last.peak ||= step.price > cheapest;
    } else {
      windows.push({ start, end: start + plan.stepSec, peak: step.price > cheapest });
    }
  });
  return windows;
}

function soakPreviewText(plan, request) {
  if (!plan.reachable) return `The heater can't reach ${request.target}° in this weather.`;
  const ready = epochTimeLabel(plan.readyAt);
  if (!request.readyAt) {
    return `Heats straight through, ready about ${ready}. About ${moneyText(plan.leaveOn.cost)} of power.`;
  }
  const parts = [];
  if (request.readyAt < plan.readyAt) {
    parts.push(`Too soon to get there: the earliest is ${ready}.`);
  } else if (plan.shortfall > 0.5) {
    parts.push(`It will only reach about ${Math.round(request.target - plan.shortfall)}° by ${ready}.`);
  }
  const hot = heatWindows(plan, plan.readyAt);
  if (hot.length) {
    const label = (w) => `${epochTimeLabel(w.start)}–${new Date(w.end * 1000).toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" })}`;
    parts.push(`Heater runs ${hot.map(label).join(", ")}${hot.some((w) => w.peak) ? " (some on-peak)" : ""}, ready ${ready}.`);
  } else {
    parts.push(`Already warm enough; ready ${ready}.`);
  }
  const saved = plan.leaveOn.cost - plan.cost;
  parts.push(`About ${moneyText(plan.cost)} of power${saved >= 0.01 ? `, ${moneyText(saved)} less than turning it up now` : ""}.`);
  return parts.join(" ");
}

async function refreshSoakPreview() {
  const request = soakRequest();
  const preview = $("#soakPreview");
  if (!(request.target >= 80 && request.target <= 104)) {
    preview.textContent = "Pick 80–104°.";
    return;
  }
  const seq = (state.soakPreviewSeq = (state.soakPreviewSeq || 0) + 1);
  preview.textContent = "Planning…";
  const params = new URLSearchParams({ target: request.target, hours: request.hours });
  if (request.readyAt) params.set("readyAt", request.readyAt);
  try {
    const plan = await requestJson(`/api/plan?${params}`);
    if (seq === state.soakPreviewSeq) preview.textContent = soakPreviewText(plan, request);
  } catch (error) {
    if (seq === state.soakPreviewSeq) preview.textContent = error.message;
  }
}

// The last soak target, remembered on this device so the next soak starts there.
const LastSoakTargetKey = "bp100g2.lastSoakTarget";

function lastSoakTarget() {
  try {
    const value = Number(localStorage.getItem(LastSoakTargetKey));
    return value >= 80 && value <= 104 ? value : null;
  } catch {
    return null;
  }
}

function openSoakSheet() {
  const data = state.data || {};
  const soak = data.smart?.soak;
  const fallback = lastSoakTarget() ?? Math.max(80, Math.min(104, Math.round(data.smart?.comfortTemp || data.setTemp || 102)));
  $("#soakTarget").value = soak?.target ?? fallback;
  $("#soakHours").value = String(Math.min(4, Math.max(1, Math.round(soak?.hours ?? 2))));
  $("#soakWhen").value = soak && !soak.asap ? "at" : "asap";
  if (soak && !soak.asap) {
    const ready = new Date(soak.readyAt * 1000);
    $("#soakTime").value = `${String(ready.getHours()).padStart(2, "0")}:${String(ready.getMinutes()).padStart(2, "0")}`;
  } else if (!$("#soakTime").value) {
    $("#soakTime").value = "19:30";
  }
  $("#soakTimeRow").hidden = $("#soakWhen").value !== "at";
  $("#soakSheetTitle").textContent = soak ? "Change Soak" : "Plan a Soak";
  $("#soakConfirm").textContent = soak ? "Update" : "Schedule";
  $("#soakSheet").classList.remove("hidden");
  refreshSoakPreview();
}

function closeSoakSheet() {
  $("#soakSheet").classList.add("hidden");
}

function setupSoak() {
  let timer = null;
  const later = () => {
    clearTimeout(timer);
    timer = setTimeout(refreshSoakPreview, SoakPreviewDelayMs);
  };
  $("#soakButton").addEventListener("click", openSoakSheet);
  $("#soakWhen").addEventListener("change", () => {
    $("#soakTimeRow").hidden = $("#soakWhen").value !== "at";
    later();
  });
  ["soakTarget", "soakTime", "soakHours"].forEach((id) => $(`#${id}`).addEventListener("input", later));
  $("#soakCancel").addEventListener("click", closeSoakSheet);
  $("#soakSheet").addEventListener("click", (event) => {
    if (event.target.id === "soakSheet") closeSoakSheet();
  });
  $("#soakConfirm").addEventListener("click", async () => {
    const request = soakRequest();
    const result = await sendCommand({ action: "soak", ...request });
    if (result) {
      try {
        localStorage.setItem(LastSoakTargetKey, String(request.target));
      } catch {
        // private mode or storage off: the sheet just won't remember
      }
      closeSoakSheet();
    }
  });
  $("#smartAction").addEventListener("click", (event) => {
    const action = event.target.dataset.action;
    // Heat Now is an as-soon-as-possible soak at the comfort temp.
    sendCommand(action === "heatNow" ? { action: "soak", target: state.data?.smart?.comfortTemp, hours: 2 } : { action });
  });
}

function spaClockLabel(spaTime) {
  const match = /^(\d{1,2}):(\d{2})$/.exec(spaTime || "");
  return match ? clock12Label(Number(match[1]), Number(match[2])) : "--";
}

function renderBanners(data) {
  $("#holdBanner").classList.toggle("hidden", !data.hold);
  $("#holdText").textContent = `Hold mode · ${data.holdMinutes} min left`;
  const reminder = data.reminder || 0;
  $("#reminderBanner").classList.toggle("hidden", reminder === 0);
  $("#reminderText").textContent =
    ReminderText[reminder] || `Spa reminder (code 0x${reminder.toString(16).padStart(2, "0")})`;
}

// ── outside temperature (Open-Meteo, fetched by the browser) ──

const WeatherCacheMs = 30 * 60 * 1000;

async function loadWeather() {
  const location = state.config?.location;
  if (!location) {
    state.weather = null;
    return;
  }
  const key = `${location.lat},${location.lon}`;
  if (state.weather?.key === key && Date.now() - state.weather.fetchedAt < WeatherCacheMs) return;
  const params = new URLSearchParams({
    latitude: location.lat,
    longitude: location.lon,
    hourly: "temperature_2m",
    daily: "temperature_2m_mean",
    temperature_unit: "fahrenheit",
    timezone: "auto",
    past_days: "14",
    forecast_days: "1",
  });
  const response = await fetch(`https://api.open-meteo.com/v1/forecast?${params}`);
  if (!response.ok) throw new Error(`Open-Meteo ${response.status}`);
  const data = await response.json();
  // Times come back in the location's local time; utc_offset_seconds turns them into Unix time.
  const offset = data.utc_offset_seconds || 0;
  const toEpoch = (local) => Date.parse(`${local}:00Z`) / 1000 - offset;
  const hourly = (data.hourly?.time || []).map((time, i) => ({ t: toEpoch(time), v: data.hourly.temperature_2m[i] }))
    .filter((point) => Number.isFinite(point.v));
  const daily = new Map();
  (data.daily?.time || []).forEach((date, i) => {
    const value = data.daily.temperature_2m_mean[i];
    if (Number.isFinite(value)) daily.set(Number(date.replaceAll("-", "")), value);
  });
  state.weather = { key, fetchedAt: Date.now(), hourly, daily };
}

// ── charts (plain SVG; colors validated against the card surface) ──

const SvgNs = "http://www.w3.org/2000/svg";

function svgEl(tag, attrs, parent) {
  const el = document.createElementNS(SvgNs, tag);
  Object.entries(attrs || {}).forEach(([name, value]) => el.setAttribute(name, value));
  if (parent) parent.append(el);
  return el;
}

function niceTicks(min, max, count = 4) {
  const raw = (max - min) / count || 1;
  const magnitude = 10 ** Math.floor(Math.log10(raw));
  const step = [1, 2, 2.5, 5, 10].map((m) => m * magnitude).find((candidate) => candidate >= raw);
  // The scale snaps outward to whole steps, and the ticks cover all of it, so no band goes unlabeled.
  const lo = Math.floor(min / step) * step;
  const hi = Math.ceil(max / step) * step;
  const ticks = [];
  for (let value = lo; value <= hi + 1e-9; value += step) ticks.push(Number(value.toFixed(6)));
  return { ticks, min: lo, max: hi };
}

// Temperature axis: the bottom is the multiple of 10 at or below the lowest reading (41 → 40, 38 → 30)
// and is always labeled; labels step up from there by 10, 20 or 50 (eight labels at most), and the top
// is the first label at least 5°F above the highest reading.
function temperatureScale(minValue, maxValue) {
  const min = Math.floor(minValue / 10) * 10;
  const top = maxValue + 5;
  const step = [10, 20, 50].find((candidate) => Math.ceil((top - min) / candidate) + 1 <= 8) || 50;
  const max = min + Math.ceil((top - min) / step) * step;
  const ticks = [];
  for (let value = min; value <= max; value += step) ticks.push(value);
  return { ticks, min, max };
}

function linePath(points, x, y, maxGap) {
  let d = "";
  points.forEach((point, i) => {
    const jump = i === 0 || point.t - points[i - 1].t > maxGap;
    d += `${jump ? "M" : "L"}${x(point.t).toFixed(1)},${y(point.v).toFixed(1)}`;
  });
  return d;
}

function chartTooltip(container) {
  let tip = container.querySelector(".chart-tip");
  if (!tip) {
    tip = document.createElement("div");
    tip.className = "chart-tip";
    tip.hidden = true;
    container.append(tip);
  }
  return tip;
}

// rows: [{ value, label, key }] where key is a CSS class for the line key (or null).
function showTip(container, tip, title, rows, anchorX) {
  const titleEl = document.createElement("div");
  titleEl.className = "tip-title";
  titleEl.textContent = title;
  const rowEls = rows.map((row) => {
    const line = document.createElement("div");
    line.className = "tip-row";
    if (row.key) {
      const key = document.createElement("i");
      key.className = `key-line ${row.key}`;
      line.append(key);
    }
    const value = document.createElement("strong");
    value.textContent = row.value;
    const label = document.createElement("span");
    label.textContent = row.label;
    line.append(value, label);
    return line;
  });
  tip.replaceChildren(titleEl, ...rowEls);
  tip.hidden = false;
  const width = tip.offsetWidth;
  const left = anchorX + 12 + width > container.clientWidth ? anchorX - 12 - width : anchorX + 12;
  tip.style.left = `${Math.max(0, left)}px`;
}

function hourLabel(epoch, withDay = false) {
  const options = { hour: "numeric" };
  if (withDay) options.weekday = "short";
  return new Date(epoch * 1000).toLocaleString(undefined, options);
}

// Smart Standby's plan: predicted water against the minimum it must stay above, heater use and the
// TOU peak behind it.
function renderPlanChart() {
  const container = $("#planChart");
  const tip = chartTooltip(container);
  container.querySelector("svg")?.remove();
  tip.hidden = true;
  const smart = state.smartPlan;
  const plan = smart?.plan;
  $("#planLegend").hidden = !plan?.steps?.length;
  if (!plan?.steps?.length) {
    $("#planSummary").textContent = "";
    $("#planNote").textContent = !smart ? "Needs a firmware update."
      : smart.mode === "lowRange" ? "Off while the spa is in the low temp range."
      : smart.mode === "manual" ? "Paused while your own temperature holds."
      : smart.mode === "idle" ? "Turn on Smart Standby in Settings → General, or plan a soak from the dashboard."
      : "Waiting for a water temperature reading to plan from.";
    return;
  }
  const compare = plan.compare;
  const hours = (plan.steps.length * plan.stepSec) / 3600;
  const saved = compare ? compare.holdCost - compare.smartCost : 0;
  $("#planSummary").textContent = !compare ? `About ${moneyText(plan.cost)} of heat for this soak`
    : saved >= 0.01 ? `About ${moneyText(saved)} less than holding ${plan.target}° over these ${Math.round(hours)} hours (≈ ${moneyText(saved * 730 / hours)} a month at this pace)`
    : `About the same as holding ${plan.target}° over these ${Math.round(hours)} hours`;
  $("#planNote").textContent = "A plan from the heating model and forecast, redone every 15 minutes. The comparison has both ending the window at the comfort temp; it counts heating only, not soaks or the cover being off.";

  const step = plan.stepSec;
  const t0 = plan.start;
  const t1 = t0 + plan.steps.length * step;
  const water = plan.steps.map((s, i) => ({ t: t0 + (i + 1) * step, v: s.temp }));
  const ready = plan.steps.map((s, i) => ({ t: t0 + i * step, v: s.min }));
  const cheapest = Math.min(...plan.steps.map((s) => s.price ?? Infinity));
  const width = container.clientWidth || 340;
  const pad = { left: 30, right: 8, top: 6 };
  const tempHeight = 120;
  const heatTop = pad.top + tempHeight + 22;
  const heatHeight = 34;
  const height = heatTop + heatHeight + 20;
  const x = (t) => pad.left + ((t - t0) / (t1 - t0)) * (width - pad.left - pad.right);
  const values = [...water.map((p) => p.v), ...ready.filter((p) => Number.isFinite(p.v)).map((p) => p.v)];
  const scale = temperatureScale(Math.min(...values) - 2, Math.max(...values) - 3);
  const y = (v) => pad.top + tempHeight - ((v - scale.min) / (scale.max - scale.min)) * tempHeight;

  const svg = svgEl("svg", { viewBox: `0 0 ${width} ${height}`, height, role: "img",
    "aria-label": "Planned spa water temperature over the next 36 hours, the minimum it must stay above, and planned heater use" });
  // Peak windows first so everything else draws over them; one band per run of peak steps.
  plan.steps.forEach((s, i) => {
    if (!(s.price > cheapest) || plan.steps[i - 1]?.price > cheapest) return;
    let end = i;
    while (plan.steps[end + 1]?.price > cheapest) end += 1;
    const left = x(t0 + i * step);
    svgEl("rect", { x: left, y: pad.top, width: x(t0 + (end + 1) * step) - left, height: heatTop + heatHeight - pad.top,
      fill: "var(--chart-peak-band)" }, svg);
  });
  scale.ticks.forEach((tick) => {
    svgEl("line", { x1: pad.left, x2: width - pad.right, y1: y(tick), y2: y(tick), stroke: "var(--chart-grid)", "stroke-width": 1 }, svg);
    svgEl("text", { x: pad.left - 6, y: y(tick) + 3, "text-anchor": "end" }, svg).textContent = `${tick}°`;
  });
  for (let t = Math.ceil(t0 / 3600) * 3600; t <= t1; t += 3600) {
    if (new Date(t * 1000).getHours() % 6 !== 0) continue;
    const label = svgEl("text", { x: x(t), y: height - 4, "text-anchor": "middle" }, svg);
    label.textContent = new Date(t * 1000).getHours() === 0 ? hourLabel(t, true).split(" ")[0] : hourLabel(t);
  }
  let readyPath = "";
  ready.forEach((p, i) => {
    if (!Number.isFinite(p.v)) return;
    const jump = i === 0 || !Number.isFinite(ready[i - 1].v);
    readyPath += `${jump ? "M" : "L"}${x(p.t).toFixed(1)},${y(p.v).toFixed(1)}H${x(p.t + step).toFixed(1)}`;
  });
  svgEl("path", { d: readyPath, fill: "none", stroke: "var(--chart-set)", "stroke-width": 1.5, "stroke-dasharray": "4 3" }, svg);
  svgEl("path", { d: linePath(water, x, y, step * 2), fill: "none", stroke: "var(--chart-water)",
    "stroke-width": 2, "stroke-linejoin": "round", "stroke-linecap": "round" }, svg);

  // Heater as minutes per clock hour, like the history chart. Per step it would show the planner's averages
  // (holding a temp exactly is a minute of heat every quarter hour), which the spa really does as one
  // longer run whenever the water drifts a degree under the set temp.
  const heatByHour = new Map();
  plan.steps.forEach((s, i) => {
    const hour = Math.floor((t0 + i * step) / 3600) * 3600;
    heatByHour.set(hour, (heatByHour.get(hour) || 0) + (s.heat || 0) * (step / 60));
  });
  svgEl("text", { x: pad.left, y: heatTop - 6, class: "panel-label" }, svg).textContent = "HEATER MIN / HOUR";
  svgEl("line", { x1: pad.left, x2: width - pad.right, y1: heatTop + heatHeight, y2: heatTop + heatHeight, stroke: "var(--chart-grid)", "stroke-width": 1 }, svg);
  svgEl("text", { x: pad.left - 6, y: heatTop + 4, "text-anchor": "end" }, svg).textContent = "60";
  heatByHour.forEach((minutes, hour) => {
    if (minutes < 0.5) return;
    const h = Math.max(2, (Math.min(minutes, 60) / 60) * heatHeight);
    const left = Math.max(pad.left, x(hour)) + 1;
    const right = Math.min(width - pad.right, x(hour + 3600));
    svgEl("rect", { x: left, y: heatTop + heatHeight - h, width: Math.max(1, right - left - 1), height: h,
      rx: 1.5, fill: "var(--chart-heater)" }, svg);
  });

  const cursor = svgEl("line", { y1: pad.top, y2: heatTop + heatHeight, stroke: "var(--muted)", "stroke-width": 1, visibility: "hidden" }, svg);
  const hit = svgEl("rect", { x: pad.left, y: 0, width: width - pad.left - pad.right, height: heatTop + heatHeight, fill: "transparent" }, svg);
  const show = (clientX) => {
    const box = svg.getBoundingClientRect();
    const t = t0 + ((clientX - box.left - pad.left) / (width - pad.left - pad.right)) * (t1 - t0);
    const i = Math.max(0, Math.min(plan.steps.length - 1, Math.floor((t - t0) / step)));
    const s = plan.steps[i];
    const cx = x(t0 + (i + 0.5) * step);
    cursor.setAttribute("x1", cx);
    cursor.setAttribute("x2", cx);
    cursor.setAttribute("visibility", "visible");
    // Whole degrees, as the spa shows them. Both round the same way, so planned never reads under minimum.
    const rows = [{ value: `${Math.round(s.temp)}°F`, label: "Planned", key: "water" }];
    if (Number.isFinite(s.min)) rows.push({ value: `${Math.round(s.min)}°F`, label: "Minimum", key: "set dashed" });
    const hourMinutes = heatByHour.get(Math.floor((t0 + i * step) / 3600) * 3600) || 0;
    rows.push({ value: `${Math.round(hourMinutes)} min`, label: "Heater this hour", key: "heater" });
    if (Number.isFinite(s.price)) rows.push({ value: `$${s.price.toFixed(3)}/kWh`, label: s.price > cheapest ? "Peak" : "Off-peak", key: null });
    showTip(container, tip, epochTimeLabel(t0 + i * step), rows, cx);
  };
  hit.addEventListener("pointermove", (event) => show(event.clientX));
  hit.addEventListener("pointerdown", (event) => show(event.clientX));
  hit.addEventListener("pointerleave", () => {
    cursor.setAttribute("visibility", "hidden");
    tip.hidden = true;
  });
  container.prepend(svg);
}

// Matches UsageModel: likely above 0.25 (ready within 30 min), possible above 0.04 (2 h), else 4 h.
function usageTier(p) {
  return p >= 0.25 ? "likely · ready within 30 min" : p >= 0.04 ? "possible · within 2 h" : "unlikely · within 4 h";
}

function renderUsageChart() {
  const container = $("#usageChart");
  const tip = chartTooltip(container);
  container.querySelector("svg")?.remove();
  tip.hidden = true;
  const usage = state.smartPlan?.usage;
  if (!usage?.workday) return;
  const rows = [["Workdays", usage.workday], ["Weekends", usage.weekend]];
  const width = container.clientWidth || 340;
  const labelWidth = 64;
  const cellHeight = 22;
  const gap = 1;
  const height = rows.length * (cellHeight + 4) + 16;
  const cell = (width - labelWidth) / 48;
  const svg = svgEl("svg", { viewBox: `0 0 ${width} ${height}`, height, role: "img",
    "aria-label": "How often a soak starts in each half hour on workdays and weekends" });
  rows.forEach(([name, values], r) => {
    const top = r * (cellHeight + 4);
    svgEl("text", { x: 0, y: top + cellHeight / 2 + 3 }, svg).textContent = name;
    values.forEach((p, slot) => {
      svgEl("rect", { x: labelWidth + slot * cell, y: top, width: Math.max(1, cell - gap), height: cellHeight, rx: 1.5,
        fill: "var(--chart-water)", "fill-opacity": (0.08 + 0.92 * Math.min(1, p / 0.5)).toFixed(2) }, svg);
    });
  });
  [0, 6, 12, 18, 24].forEach((hour) => {
    const label = svgEl("text", { x: labelWidth + hour * 2 * cell, y: height - 2,
      "text-anchor": hour === 0 ? "start" : hour === 24 ? "end" : "middle" }, svg);
    label.textContent = ["12 AM", "6 AM", "12 PM", "6 PM", "12 AM"][hour / 6];
  });
  const hit = svgEl("rect", { x: labelWidth, y: 0, width: width - labelWidth, height: rows.length * (cellHeight + 4), fill: "transparent" }, svg);
  const show = (clientX, clientY) => {
    const box = svg.getBoundingClientRect();
    const slot = Math.max(0, Math.min(47, Math.floor((clientX - box.left - labelWidth) / cell)));
    const r = Math.max(0, Math.min(rows.length - 1, Math.floor((clientY - box.top) / (cellHeight + 4))));
    const p = rows[r][1][slot];
    const time = clock12Label(Math.floor(slot / 2), (slot % 2) * 30);
    showTip(container, tip, `${rows[r][0]} ${time}`, [
      { value: `${Math.round(p * 100)}%`, label: "of days", key: null },
      { value: usageTier(p), label: "", key: null },
    ], labelWidth + (slot + 0.5) * cell);
  };
  hit.addEventListener("pointermove", (event) => show(event.clientX, event.clientY));
  hit.addEventListener("pointerdown", (event) => show(event.clientX, event.clientY));
  hit.addEventListener("pointerleave", () => { tip.hidden = true; });
  container.prepend(svg);
}

// What the last-48-hours chart's "now" point shows, to redraw it only when that changes.
function hoursLiveKey(data) {
  return data ? `${data.valid}/${data.waterTemp}/${data.setTemp}/${!!data.heating}` : "";
}

function renderHoursChart() {
  const container = $("#hoursChart");
  state.hoursChartLive = hoursLiveKey(state.data);
  const hours = state.runtimeHours || [];
  const tip = chartTooltip(container);
  container.querySelector("svg")?.remove();
  tip.hidden = true;
  if (hours.length < 2) {
    $("#hoursNote").textContent = "The chart fills in as the controller records each hour.";
    $("#outsideKey").hidden = true;
    return;
  }
  const t0 = hours[0].start;
  const lastStart = hours[hours.length - 1].start;
  // Hours are averages, so the current hour ends in a live "now" point with what the spa reports.
  const now = Math.floor(Date.now() / 1000);
  const live = state.data?.valid && now > lastStart && now < lastStart + 7200
    ? { start: now, live: true, waterTemp: state.data.waterTemp, setTemp: state.data.setTemp, heating: !!state.data.heating }
    : null;
  const t1 = live ? now : lastStart + 3600;
  const end = (hour) => Math.min(hour.start + 3600, t1);
  // Each average sits mid-way through the time it covers, so the current hour's stays before "now".
  const water = hours.filter((h) => Number.isFinite(h.waterTemp)).map((h) => ({ t: (h.start + end(h)) / 2, v: h.waterTemp }));
  if (Number.isFinite(live?.waterTemp)) water.push({ t: now, v: live.waterTemp });
  const setPoints = hours.filter((h) => Number.isFinite(h.setTemp)).map((h) => ({ t: h.start, v: h.setTemp }));
  const outside = (state.weather?.hourly || []).filter((p) => p.t >= t0 && p.t <= t1);
  $("#outsideKey").hidden = !outside.length;
  $("#hoursNote").textContent = !state.config?.location
    ? "Set your location in Settings → General to compare with the outside temperature."
    : outside.length ? "" : "Outside temperature isn't available right now.";

  const width = container.clientWidth || 340;
  const pad = { left: 30, right: 14, top: 6 };  // the right margin is where "now" is read
  const tempHeight = 130;
  const heatTop = pad.top + tempHeight + 22;
  const heatHeight = 34;
  const height = heatTop + heatHeight + 20;
  const x = (t) => pad.left + ((t - t0) / (t1 - t0)) * (width - pad.left - pad.right);
  const liveSet = Number.isFinite(live?.setTemp) ? live.setTemp : NaN;
  const values = [...water, ...setPoints, ...outside].map((p) => p.v).concat(Number.isFinite(liveSet) ? [liveSet] : []);
  const scale = temperatureScale(Math.min(...values), Math.max(...values));
  const y = (v) => pad.top + tempHeight - ((v - scale.min) / (scale.max - scale.min)) * tempHeight;

  const svg = svgEl("svg", { viewBox: `0 0 ${width} ${height}`, height, role: "img",
    "aria-label": "Spa water and outside temperature over the last 48 hours, with heater minutes per hour" });
  scale.ticks.forEach((tick) => {
    svgEl("line", { x1: pad.left, x2: width - pad.right, y1: y(tick), y2: y(tick), stroke: "var(--chart-grid)", "stroke-width": 1 }, svg);
    svgEl("text", { x: pad.left - 6, y: y(tick) + 3, "text-anchor": "end" }, svg).textContent = `${tick}°`;
  });
  // x ticks every 6 local hours
  const firstTick = Math.ceil(t0 / 3600) * 3600;
  for (let t = firstTick; t <= t1; t += 3600) {
    if (new Date(t * 1000).getHours() % 6 !== 0) continue;
    const label = svgEl("text", { x: x(t), y: height - 4, "text-anchor": "middle" }, svg);
    label.textContent = new Date(t * 1000).getHours() === 0 ? hourLabel(t, true).split(" ")[0] : hourLabel(t);
  }
  if (outside.length) {
    svgEl("path", { d: linePath(outside, x, y, 7200), fill: "none", stroke: "var(--chart-outside)",
      "stroke-width": 2, "stroke-linejoin": "round", "stroke-linecap": "round" }, svg);
  }
  // Set temperature changes in steps, so it's drawn as one: flat across each hour, straight up or down
  // where it changed. Drawn under the water line, which usually sits right on it.
  let setPath = "";
  setPoints.forEach((point, i) => {
    const jump = i === 0 || point.t - setPoints[i - 1].t > 3600;
    setPath += `${jump ? "M" : "L"}${x(point.t).toFixed(1)},${y(point.v).toFixed(1)}H${x(Math.min(point.t + 3600, t1)).toFixed(1)}`;
  });
  if (setPath && Number.isFinite(liveSet)) setPath += `V${y(liveSet).toFixed(1)}`;  // ends at the set temp now
  if (setPath) {
    svgEl("path", { d: setPath, fill: "none", stroke: "var(--chart-set)", "stroke-width": 1.5, "stroke-linejoin": "round" }, svg);
  }
  if (Number.isFinite(liveSet)) {
    svgEl("circle", { cx: x(now), cy: y(liveSet), r: 3, fill: "var(--chart-set)", stroke: "var(--surface)", "stroke-width": 1.5 }, svg);
  }
  svgEl("path", { d: linePath(water, x, y, 5400), fill: "none", stroke: "var(--chart-water)",
    "stroke-width": 2, "stroke-linejoin": "round", "stroke-linecap": "round" }, svg);
  const last = water[water.length - 1];
  if (last) svgEl("circle", { cx: x(last.t), cy: y(last.v), r: 4, fill: "var(--chart-water)", stroke: "var(--surface)", "stroke-width": 2 }, svg);

  // heater strip: minutes per hour on its own 0–60 scale
  svgEl("text", { x: pad.left, y: heatTop - 6, class: "panel-label" }, svg).textContent = "HEATER MIN / HOUR";
  svgEl("line", { x1: pad.left, x2: width - pad.right, y1: heatTop + heatHeight, y2: heatTop + heatHeight, stroke: "var(--chart-grid)", "stroke-width": 1 }, svg);
  svgEl("text", { x: pad.left - 6, y: heatTop + 4, "text-anchor": "end" }, svg).textContent = "60";
  hours.forEach((hour) => {
    if (!hour.heatingSec) return;
    const h = Math.max(2, (Math.min(hour.heatingSec, 3600) / 3600) * heatHeight);
    const left = x(hour.start) + 1;
    svgEl("rect", { x: left, y: heatTop + heatHeight - h, width: Math.max(1, x(end(hour)) - left - 1), height: h,
      rx: 1.5, fill: "var(--chart-heater)" }, svg);
  });

  const cursor = svgEl("line", { y1: pad.top, y2: heatTop + heatHeight, stroke: "var(--muted)", "stroke-width": 1, visibility: "hidden" }, svg);
  const hit = svgEl("rect", { x: pad.left, y: 0, width: width - pad.left, height: heatTop + heatHeight, fill: "transparent" }, svg);
  const nearestOutside = (t) => outside.reduce((best, p) => (!best || Math.abs(p.t - t) < Math.abs(best.t - t) ? p : best), null);
  const center = (h) => (h.live ? now : (h.start + end(h)) / 2);
  const show = (clientX) => {
    const box = svg.getBoundingClientRect();
    const px = clientX - box.left;
    const t = t0 + ((px - pad.left) / (width - pad.left - pad.right)) * (t1 - t0);
    // At the right edge and past it: "now". An hour there is only a few pixels wide on a phone.
    const hour = live && px >= x(now) - 1 ? live
      : hours.reduce((best, h) => (Math.abs(center(h) - t) < Math.abs(center(best) - t) ? h : best), hours[0]);
    const cx = x(center(hour));
    cursor.setAttribute("x1", cx);
    cursor.setAttribute("x2", cx);
    cursor.setAttribute("visibility", "visible");
    const out = nearestOutside(center(hour));
    const rows = [
      { value: Number.isFinite(hour.waterTemp) ? `${hour.waterTemp.toFixed(1)}°F` : "--", label: "Spa", key: "water" },
      { value: Number.isFinite(hour.setTemp) ? `${Math.round(hour.setTemp)}°F` : "--", label: "Set", key: "set" },
    ];
    if (outside.length) rows.push({ value: out && Math.abs(out.t - center(hour)) <= 3600 ? `${Math.round(out.v)}°F` : "--", label: "Outside", key: "outside" });
    rows.push(hour.live ? { value: hour.heating ? "On" : "Off", label: "Heater", key: "heater" }
      : { value: `${Math.round(hour.heatingSec / 60)} min`, label: end(hour) < hour.start + 3600 ? "Heater so far" : "Heater", key: "heater" });
    showTip(container, tip, hour.live ? "Now" : hourLabel(hour.start, true), rows, cx);
  };
  hit.addEventListener("pointermove", (event) => show(event.clientX));
  hit.addEventListener("pointerdown", (event) => show(event.clientX));
  hit.addEventListener("pointerleave", () => {
    cursor.setAttribute("visibility", "hidden");
    tip.hidden = true;
  });
  container.prepend(svg);
}

const TrendMinDays = 5;
const TrendMinSpreadF = 5;

// Least-squares fit of heater minutes per day against the day's temperature gap (average spa water minus
// average outside). Heat loss follows that gap, so turning the spa down on a cold day doesn't read as
// "cold days need less heating". Uses finished days that have both averages; null without a location.
function heaterTrend() {
  const daily = state.weather?.daily;
  if (!daily) return null;
  const now = new Date();
  const today = now.getFullYear() * 10000 + (now.getMonth() + 1) * 100 + now.getDate();
  const points = (state.runtimeDays || [])
    .filter((day) => day.date !== today && daily.has(day.date) && Number.isFinite(day.waterTemp))
    .map((day) => ({ x: day.waterTemp - daily.get(day.date), y: day.heatingSec / 60 }));
  const n = points.length;
  if (n < TrendMinDays) return { n };
  const xs = points.map((p) => p.x);
  if (Math.max(...xs) - Math.min(...xs) < TrendMinSpreadF) return { n, flat: true };
  const meanX = xs.reduce((a, b) => a + b, 0) / n;
  const meanY = points.reduce((a, p) => a + p.y, 0) / n;
  let sxx = 0;
  let sxy = 0;
  let syy = 0;
  points.forEach((p) => {
    sxx += (p.x - meanX) ** 2;
    sxy += (p.x - meanX) * (p.y - meanY);
    syy += (p.y - meanY) ** 2;
  });
  return { n, slope: sxy / sxx, r: syy ? sxy / Math.sqrt(sxx * syy) : 0 };
}

function renderDailySummary() {
  const summary = $("#dailySummary");
  const trend = heaterTrend();
  const parts = [];
  const muted = (text) => {
    const span = document.createElement("span");
    span.className = "muted";
    span.textContent = text;
    return span;
  };
  if (!trend) {
    summary.replaceChildren();
    return;
  }
  if (trend.slope === undefined) {
    parts.push(muted(trend.flat
      ? "Outside temperature hasn't varied enough yet to compare with heater time."
      : `Needs ${TrendMinDays} full days with spa and outside temperatures to compare (${trend.n} so far).`));
  } else if (trend.r < 0.3) {
    // Enough days, but a bigger spa-to-outside gap didn't clearly run the heater longer: no honest rate.
    parts.push(document.createTextNode(`Heater time hasn't followed outside temperature over these ${trend.n} days`));
  } else {
    // A 10°F colder day at the same spa temperature is a 10°F bigger gap.
    const perTen = Math.round(trend.slope * 10);
    const amount = perTen >= 60 ? `${Math.floor(perTen / 60)}h ${String(perTen % 60).padStart(2, "0")}m` : `${perTen} min`;
    parts.push(document.createTextNode(`About +${amount} of heater per day for every 10°F colder`));
    parts.push(muted(` · adjusted for spa temperature · ${trend.n} days`));
  }
  summary.replaceChildren(...parts);
}

// Bar with a rounded data end and a square baseline.
function barPath(x, y, width, height, radius = 4) {
  const r = Math.min(radius, width / 2, height);
  return `M${x},${y + height}V${y + r}Q${x},${y} ${x + r},${y}H${x + width - r}` +
    `Q${x + width},${y} ${x + width},${y + r}V${y + height}Z`;
}

function renderDailyChart() {
  const container = $("#dailyChart");
  const tip = chartTooltip(container);
  container.querySelector("svg")?.remove();
  tip.hidden = true;
  const days = (state.runtimeDays || []).slice(-14);
  if (days.length < 2) {
    $("#dailyNote").textContent = "Fills in after a couple of days of history.";
    return;
  }
  const now = new Date();
  const today = now.getFullYear() * 10000 + (now.getMonth() + 1) * 100 + now.getDate();
  const daily = state.weather?.daily;
  const outside = days.map((day, i) => ({ i, v: daily?.get(day.date) })).filter((p) => Number.isFinite(p.v));
  $("#dailyNote").textContent = !state.config?.location
    ? "Set your location in Settings → General to add outside temperature."
    : outside.length ? "" : "Outside temperature isn't available right now.";

  const width = container.clientWidth || 340;
  const pad = { left: 30, right: 8 };
  const heatTop = 18;
  const heatHeight = 96;
  const outTop = heatTop + heatHeight + 30;
  const outHeight = outside.length ? 78 : 0;
  const height = (outside.length ? outTop + outHeight : heatTop + heatHeight) + 22;
  const plotWidth = width - pad.left - pad.right;
  const band = plotWidth / days.length;
  const cx = (i) => pad.left + band * (i + 0.5);

  const svg = svgEl("svg", { viewBox: `0 0 ${width} ${height}`, height, role: "img",
    "aria-label": "Heater hours per day over the last 14 days, with average outside temperature per day" });

  // heater hours per day
  const hours = days.map((day) => day.heatingSec / 3600);
  const hourScale = niceTicks(0, Math.max(1, ...hours), 3);
  const yHeat = (v) => heatTop + heatHeight - (v / hourScale.max) * heatHeight;
  svgEl("text", { x: pad.left, y: heatTop - 8, class: "panel-label" }, svg).textContent = "HEATER HOURS";
  hourScale.ticks.forEach((tick) => {
    svgEl("line", { x1: pad.left, x2: width - pad.right, y1: yHeat(tick), y2: yHeat(tick), stroke: "var(--chart-grid)", "stroke-width": 1 }, svg);
    svgEl("text", { x: pad.left - 6, y: yHeat(tick) + 3, "text-anchor": "end" }, svg).textContent = `${tick}h`;
  });
  const barWidth = Math.min(24, band * 0.6);
  days.forEach((day, i) => {
    const h = heatTop + heatHeight - yHeat(hours[i]);
    if (h <= 0) return;
    svgEl("path", { d: barPath(cx(i) - barWidth / 2, yHeat(hours[i]), barWidth, h), fill: "var(--chart-heater)",
      opacity: day.date === today ? 0.5 : 1 }, svg);
  });

  // average outside temperature per day
  let yOut = null;
  if (outside.length) {
    const scale = temperatureScale(Math.min(...outside.map((p) => p.v)), Math.max(...outside.map((p) => p.v)));
    yOut = (v) => outTop + outHeight - ((v - scale.min) / (scale.max - scale.min)) * outHeight;
    svgEl("text", { x: pad.left, y: outTop - 8, class: "panel-label" }, svg).textContent = "OUTSIDE AVG °F";
    scale.ticks.forEach((tick) => {
      svgEl("line", { x1: pad.left, x2: width - pad.right, y1: yOut(tick), y2: yOut(tick), stroke: "var(--chart-grid)", "stroke-width": 1 }, svg);
      svgEl("text", { x: pad.left - 6, y: yOut(tick) + 3, "text-anchor": "end" }, svg).textContent = `${tick}°`;
    });
    const d = outside.map((p, k) => `${k ? "L" : "M"}${cx(p.i).toFixed(1)},${yOut(p.v).toFixed(1)}`).join("");
    svgEl("path", { d, fill: "none", stroke: "var(--chart-outside)", "stroke-width": 2, "stroke-linejoin": "round", "stroke-linecap": "round" }, svg);
    outside.forEach((p) => {
      svgEl("circle", { cx: cx(p.i), cy: yOut(p.v), r: 4, fill: "var(--chart-outside)", stroke: "var(--surface)", "stroke-width": 2 }, svg);
    });
  }

  // day labels; every other day when the columns are narrow
  const every = band >= 34 ? 1 : 2;
  days.forEach((day, i) => {
    if ((days.length - 1 - i) % every !== 0) return;
    const label = day.date === today ? "Today" : `${Math.floor(day.date / 100) % 100}/${day.date % 100}`;
    svgEl("text", { x: cx(i), y: height - 4, "text-anchor": "middle" }, svg).textContent = label;
  });

  // one readout per day, across both panels
  const bottom = outside.length ? outTop + outHeight : heatTop + heatHeight;
  const cursor = svgEl("line", { y1: heatTop, y2: bottom, stroke: "var(--muted)", "stroke-width": 1, visibility: "hidden" }, svg);
  days.forEach((day, i) => {
    const column = svgEl("rect", { x: pad.left + band * i, y: heatTop, width: band, height: bottom - heatTop,
      fill: "transparent", tabindex: 0, "aria-label": `${dayLabel(day.date)}: heater ${hoursMinutes(day.heatingSec)}` }, svg);
    const show = () => {
      cursor.setAttribute("x1", cx(i));
      cursor.setAttribute("x2", cx(i));
      cursor.setAttribute("visibility", "visible");
      const out = daily?.get(day.date);
      const rows = [{ value: hoursMinutes(day.heatingSec), label: day.date === today ? "Heater so far" : "Heater", key: "heater" }];
      if (outside.length) rows.push({ value: Number.isFinite(out) ? `${Math.round(out)}°F` : "--", label: "Outside avg", key: "outside" });
      if (Number.isFinite(day.setTemp)) rows.push({ value: `${Math.round(day.setTemp)}°F`, label: "Set avg", key: "set" });
      showTip(container, tip, dayLabel(day.date), rows, cx(i));
    };
    column.addEventListener("pointerenter", show);
    column.addEventListener("pointerdown", show);
    column.addEventListener("focus", show);
    const hide = () => {
      cursor.setAttribute("visibility", "hidden");
      tip.hidden = true;
    };
    column.addEventListener("pointerleave", hide);
    column.addEventListener("blur", hide);
  });
  container.prepend(svg);
}

function hoursMinutes(seconds) {
  const totalMinutes = Math.round((seconds || 0) / 60);
  const hours = Math.floor(totalMinutes / 60);
  const minutes = totalMinutes % 60;
  return hours ? `${hours}h ${String(minutes).padStart(2, "0")}m` : `${minutes}m`;
}

// "Mon 26": compact enough for a seven-column table on a phone.
function shortDayLabel(date) {
  const day = new Date(Math.floor(date / 10000), (Math.floor(date / 100) % 100) - 1, date % 100);
  return `${day.toLocaleDateString(undefined, { weekday: "short" })} ${date % 100}`;
}

function dayLabel(date) {
  const year = Math.floor(date / 10000);
  const month = Math.floor(date / 100) % 100;
  const day = date % 100;
  return new Date(year, month - 1, day).toLocaleDateString(undefined, { weekday: "short", month: "short", day: "numeric" });
}

async function loadRuntime() {
  const [message, thermal, smart] = await Promise.all([
    requestJson("/api/runtime"),
    requestJson("/api/thermal").catch((error) => console.warn("[bp100g2] thermal failed", error)),
    requestJson("/api/smart").catch((error) => console.warn("[bp100g2] smart plan failed", error)),
    loadWeather().catch((error) => console.warn("[bp100g2] weather failed", error)),
  ]);
  state.thermal = thermal || null;
  state.smartPlan = smart || null;
  state.runtimeDays = message.days || [];
  state.runtimeHours = message.hours || [];
  state.runtimeLoadedAt = Date.now();
  if (state.data) renderOverview(state.data, state.data.capabilities || {});
  renderRuntime();
}

function hoursMinutesShort(seconds) {
  const total = Math.round((seconds || 0) / 60);
  return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, "0")}`;
}

function renderRuntime() {
  const days = (state.runtimeDays || []).slice().reverse().slice(0, 14);
  const outside = state.weather?.daily || new Map();
  const body = $("#runtimeTable tbody");
  body.replaceChildren();
  if (!days.length) {
    const row = body.insertRow();
    const cell = row.insertCell();
    cell.colSpan = 8;
    cell.className = "empty";
    cell.textContent = "No runtime recorded yet";
  }
  renderDailySummary();
  days.forEach((day) => {
    const row = body.insertRow();
    const out = outside.get(day.date);
    const temp = (value) => (Number.isFinite(value) ? Math.round(value) : "--");
    [shortDayLabel(day.date), hoursMinutesShort(day.heatingSec), hoursMinutesShort(day.pumpLowSec + day.pumpHighSec),
     hoursMinutesShort(day.filterSec), temp(day.waterTemp), temp(day.setTemp), temp(out)].forEach((text) => {
      row.insertCell().textContent = text;
    });
    const cost = row.insertCell();
    cost.className = "cost-col";
    cost.textContent = ratesSet() ? moneyText(dayEnergy(day).cost) : "";
  });
  $("#runtimeTable").classList.toggle("no-cost", !ratesSet());
  renderCostCard();
  renderThermal();
  if ($("#statsRuntime").classList.contains("active") && $("#stats").classList.contains("active")) {
    renderPlanChart();
    renderUsageChart();
    renderHoursChart();
    renderDailyChart();
  }
}

// ── heating model (learned by the controller from the tub's own heating and cooling) ──

const ThermalRunsToLearn = 5;  // matches the firmware: below this the starting estimate still counts
const ThermalFallbackOutside = 50;

// Same physics as the firmware: dT/dt = heater - loss * (water - outside).
function thermalHoursToHeat(model, from, to, outside) {
  if (!(to > from)) return 0;
  const level = outside + model.heaterRate / model.lossCoeff;
  if (to >= level) return Infinity;
  return Math.log((level - from) / (level - to)) / model.lossCoeff;
}

function thermalLearnedText(count) {
  if (count >= ThermalRunsToLearn) return "measured";
  return count ? `estimate, ${count} of ${ThermalRunsToLearn} runs` : "estimate";
}

function renderThermal() {
  const card = $("#thermalCard");
  const body = $("#thermalTable tbody");
  body.replaceChildren();
  const thermal = state.thermal;
  if (!thermal?.model) {
    $("#thermalNowCard").replaceChildren();
    card.replaceChildren(maintRow("Needs a firmware update", "", "muted"));
    $("#thermalHint").textContent = "";
    $("#thermalRunsMeta").textContent = "None yet";
    return;
  }
  const { model, runs = {}, weather = {} } = thermal;
  const heatRuns = runs.heat || [];
  const coolRuns = runs.cool || [];
  const outside = weather.now;
  const air = Number.isFinite(outside) ? outside : ThermalFallbackOutside;
  const data = state.data || {};
  const water = waterReading(data).temp;
  // Right now: the learned numbers applied to today's water and weather.
  const now = [];
  if (!weather.hasLocation) {
    now.push(maintRow("Outside", "Set a location in Settings", "due"));
  } else if (Number.isFinite(outside)) {
    now.push(maintRow("Outside", `${Math.round(outside)}°F · forecast ${ageText(weather.fetchedAt)}`));
  } else {
    now.push(maintRow("Outside", weather.error || "Waiting for the forecast", "muted"));
  }
  if (Number.isFinite(water)) {
    const loss = model.lossCoeff * (water - air);
    now.push(maintRow("Heater On", `rises ${(model.heaterRate - loss).toFixed(1)}°F/hr`));
    now.push(maintRow("Heater Off", `drops ${loss.toFixed(1)}°F/hr`));
    if (Number.isFinite(data.setTemp) && data.setTemp > water) {
      const hours = thermalHoursToHeat(model, water, data.setTemp, air);
      now.push(maintRow(`To ${Math.round(data.setTemp)}°F`, Number.isFinite(hours) ? `about ${hoursMinutes(hours * 3600)}` : "Heater can't keep up"));
    }
  }
  $("#thermalNowCard").replaceChildren(...now);
  // Learned: properties of the tub itself, which only change as runs are measured.
  card.replaceChildren(
    maintRow("Heater Power", `${model.heaterRate.toFixed(1)}°F/hr · ${thermalLearnedText(heatRuns.length)}`),
    maintRow("Heat Loss", `${(model.lossCoeff * 60).toFixed(1)}°F/hr · ${thermalLearnedText(coolRuns.length)}`),
  );
  $("#thermalHint").textContent = "Heater power is how fast the heater alone warms the water, before any heat loss. " +
    "Heat loss is how fast it cools with the heater off, at 100°F water and 40°F outside so it compares across " +
    "seasons; right now it's more or less depending on how much warmer the water is than the air. Both are learned " +
    "from the tub's own heating and cool-downs (runs while it's in use are skipped). Until then they're estimated " +
    "from the heater kW and tub volume in Settings → Energy.";

  const all = [...heatRuns.map((run) => ({ ...run, kind: "Heat" })), ...coolRuns.map((run) => ({ ...run, kind: "Cool" }))]
    .sort((a, b) => b.t - a.t);
  $("#thermalRunsMeta").textContent = all.length ? `${all.length} runs` : "None yet";
  if (!all.length) {
    const cell = body.insertRow().insertCell();
    cell.colSpan = 6;
    cell.className = "empty";
    cell.textContent = "No runs measured yet";
  }
  all.forEach((run) => {
    const row = body.insertRow();
    const when = new Date(run.t * 1000);
    const rate = run.kind === "Heat" ? run.v : run.v * 60;
    [`${when.toLocaleDateString(undefined, { weekday: "short" })} ${when.toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" })}`,
     run.kind, `${Math.round(run.a)}→${Math.round(run.b)}`, hoursMinutesShort(run.m * 60),
     Number.isFinite(run.o) ? Math.round(run.o) : "--", rate.toFixed(1)].forEach((text) => {
      row.insertCell().textContent = text;
    });
  });
}

function infoRow(label, value) {
  const row = document.createElement("div");
  row.className = "form-row";
  const name = document.createElement("span");
  name.className = "form-row-label";
  name.textContent = label;
  const text = document.createElement("strong");
  text.className = "form-row-value";
  text.textContent = value;
  row.append(name, text);
  return row;
}

function equipmentSummary(equipment) {
  const parts = [];
  (equipment.pumps || []).forEach((speeds, index) => {
    if (speeds) parts.push(`Pump ${index + 1} (${speeds === 2 ? "2-speed" : "1-speed"})`);
  });
  if (equipment.light1) parts.push("Light");
  if (equipment.light2) parts.push("Light 2");
  if (equipment.blower) parts.push("Blower");
  if (equipment.circulationPump) parts.push("Circulation pump");
  if (equipment.aux1) parts.push("Aux 1");
  if (equipment.aux2) parts.push("Aux 2");
  if (equipment.mister) parts.push("Mister");
  return parts.join(", ") || "None reported";
}

async function loadSpaInfo() {
  const info = await requestJson("/api/spa-info");
  const rows = [];
  if (info.system) {
    rows.push(infoRow("Model", info.system.model || "--"));
    rows.push(infoRow("Software", info.system.softwareVersion));
    if (info.system.heaterVoltage) rows.push(infoRow("Heater", `${info.system.heaterVoltage} V`));
    rows.push(infoRow("DIP switches", info.system.dipSwitches));
  }
  if (info.equipment) rows.push(infoRow("Equipment", equipmentSummary(info.equipment)));
  if (info.tempRanges) {
    const r = info.tempRanges;
    rows.push(infoRow("Temp ranges", `Low ${r.lowMin}–${r.lowMax}°F · High ${r.highMin}–${r.highMax}°F`));
  }
  const container = $("#spaInfo");
  container.replaceChildren(...(rows.length ? rows : [infoRow("Waiting for the spa…", "")]));
}

// The spa reports 255 days ago for entries too old (or too unclear) to date.
function faultDaysText(daysAgo) {
  if (daysAgo >= 255) return "A while ago";
  return daysAgo === 0 ? "Today" : daysAgo === 1 ? "Yesterday" : `${daysAgo} days ago`;
}

function faultWhen(entry) {
  return entry.daysAgo >= 255 ? faultDaysText(entry.daysAgo) : `${faultDaysText(entry.daysAgo)} at ${clock12Label(entry.hour, entry.minute)}`;
}

async function loadFaults() {
  const log = await requestJson("/api/faults");
  const entries = (log.entries || []).slice().sort((a, b) => a.daysAgo - b.daysAgo || b.hour - a.hour || b.minute - a.minute);
  state.faultLog = { read: (log.seq ?? 0) > 0, entries };
  if (state.data) renderOverview(state.data, state.data.capabilities || {});
  const list = $("#faultList");
  list.replaceChildren();
  if (!state.faultLog.read) {
    $("#faultLogState").textContent = "Not read yet";
    return;
  }
  $("#faultLogState").textContent = `${entries.length} ${entries.length === 1 ? "fault" : "faults"}`;
  if (!entries.length) {
    const item = document.createElement("li");
    item.className = "empty";
    item.textContent = "No faults recorded";
    list.append(item);
    return;
  }
  entries.forEach((entry) => {
    const item = document.createElement("li");
    item.textContent = FaultText[entry.code] || `Fault code ${entry.code}`;
    const meta = document.createElement("span");
    meta.className = "fault-meta";
    meta.textContent = `${faultWhen(entry)} · set ${entry.setTemp}°F · sensors ${entry.sensorA}°/${entry.sensorB}°F`;
    item.append(meta);
    list.append(item);
  });
}

function currentTargetTemp() {
  const value = state.data?.setTemp;
  return Number.isFinite(value) ? clampTemp(value) : 100;
}

function renderModalTemp() {
  $("#modalValue").textContent = tempLabel(state.modalTemp);
}

function openTempModal() {
  state.modalTemp = currentTargetTemp();
  renderModalTemp();
  $("#tempModal").classList.remove("hidden");
}

function closeTempModal() {
  $("#tempModal").classList.add("hidden");
}

function adjustModalTemp(delta) {
  state.modalTemp = clampTemp((state.modalTemp ?? currentTargetTemp()) + delta);
  renderModalTemp();
}

function setCommandModal(mode, title, details = "") {
  const modal = $("#commandModal");
  state.commandModalOpen = true;
  state.commandModalStatus = mode;
  $("#commandTitle").textContent = title;
  $("#commandDetails").textContent = details;
  $("#commandSpinner").classList.toggle("hidden", mode !== "pending");
  $("#commandSuccess").classList.toggle("hidden", mode !== "success");
  $("#commandClose").classList.toggle("hidden", mode !== "error");
  modal.classList.remove("hidden");
  modal.setAttribute("aria-hidden", "false");
}

function hideCommandModal() {
  if (state.commandSuccessTimer) {
    clearTimeout(state.commandSuccessTimer);
    state.commandSuccessTimer = null;
  }
  state.commandModalOpen = false;
  state.commandModalStatus = "idle";
  clearCommandWait();
  state.commandResolve = null;
  $("#commandModal").classList.add("hidden");
  $("#commandModal").setAttribute("aria-hidden", "true");
}

function completeCommandModal() {
  setCommandModal("success", "Success!");
  const resolve = state.commandResolve;
  clearCommandWait();
  state.commandResolve = null;
  if (resolve) {
    resolve();
  }
  state.commandSuccessTimer = setTimeout(hideCommandModal, 1600);
}

function failCommandModal(message) {
  setCommandModal("error", "Command failed", message);
  clearCommandWait();
  const resolve = state.commandResolve;
  state.commandResolve = null;
  if (resolve) {
    resolve();
  }
}

function commandPresentation(command) {
  switch (command.action) {
    case "setTemp":
      return { title: "Setting Temperature", details: `Target: ${tempLabel(command.value)}` };
    case "setFilter1":
      return { title: "Setting Filter 1", details: filterSummary(command) };
    case "setFilter2":
      return { title: "Setting Filter 2", details: filterSummary(command) };
    case "jet1":
      return { title: "Applying Jet 1", details: `Current: ${jetLabel(state.data?.jet1, state.data?.jet1Speed)}` };
    case "jet2":
      return { title: "Applying Jet 2", details: `Current: ${jetLabel(state.data?.jet2, state.data?.jet2Speed)}` };
    case "light":
      return { title: "Applying Light", details: `Current: ${state.data?.light ? "On" : "Off"}` };
    case "syncTime":
      return { title: "Setting Spa Time", details: "Syncing controller time" };
    case "tempRange":
      return { title: "Switching Range", details: command.value === "high" ? "High range" : "Low range" };
    case "startStatusCapture":
      return { title: "Starting Capture", details: "Recording status frames" };
    case "stopStatusCapture":
      return { title: "Stopping Capture", details: "Capture will remain available" };
    case "clearStatusCapture":
      return { title: "Clearing Capture", details: "Removing captured frames" };
    default:
      return { title: "Applying Command", details: "" };
  }
}

function commandWaitType(command) {
  return ["setFilter1", "setFilter2"].includes(command.action) ? "filter" : null;
}

function usesBlockingCommandModal(command) {
  return ["setTemp", "setFilter1", "setFilter2"].includes(command.action);
}

const FilterConfirmTimeoutMs = 8000;
// Actions whose result is visible on the dashboard; only failures need a toast.
const QuietActions = new Set(["togglePump", "setPump", "light", "hold", "readFaultLog"]);

function waitForCommandCompletion(type, command, startSeq) {
  const timer = setTimeout(() => {
    failCommandModal("The spa did not confirm the filter change.\nCheck the spa panel, then try again.");
  }, FilterConfirmTimeoutMs);
  state.commandWait = { type, command, startSeq, retried: false, timer };
  return new Promise((resolve) => {
    state.commandResolve = resolve;
  });
}

function clearCommandWait() {
  if (state.commandWait?.timer) {
    clearTimeout(state.commandWait.timer);
  }
  state.commandWait = null;
}

// Returns a description of what the spa reported if it differs from the command, otherwise null.
function filterMismatch(command, filters) {
  const cycle = command.action === "setFilter2" ? 2 : 1;
  const wantEnabled = cycle === 1 || !!command.enabled;
  const reported = {
    enabled: cycle === 1 || !!filters.cycle2Enabled,
    startHour: filters[`cycle${cycle}Start`],
    startMinute: filters[`cycle${cycle}StartMinute`],
    durationHour: filters[`cycle${cycle}Duration`],
    durationMinute: filters[`cycle${cycle}DurationMinute`],
  };
  if (reported.enabled !== wantEnabled) {
    return `filter 2 is ${reported.enabled ? "on" : "off"}`;
  }
  if (!wantEnabled) {
    return null;
  }
  const matches = ["startHour", "startMinute", "durationHour", "durationMinute"].every(
    (key) => reported[key] === command[key]
  );
  return matches ? null : `filter ${cycle} is set to:\n${filterSummary(reported)}`;
}

function updateCommandWait(data) {
  const wait = state.commandWait;
  if (wait?.type !== "filter") return;
  const seq = data.filterCycles?.seq ?? 0;
  if (seq <= wait.startSeq) return;
  wait.startSeq = seq;
  const mismatch = filterMismatch(wait.command, data.filterCycles);
  if (!mismatch) {
    completeCommandModal();
  } else if (!wait.retried) {
    // The read-back can race the spa applying the change; read once more before failing.
    wait.retried = true;
    requestJson("/api/cmd", { method: "POST", body: JSON.stringify({ action: "readFilterCycles" }) }).catch(() => {});
  } else {
    failCommandModal(`The spa reports ${mismatch}`);
  }
}

async function requestJson(url, options = {}) {
  const response = await fetch(url, {
    headers: { "content-type": "application/json" },
    ...options,
  });
  const text = await response.text();
  const body = text ? JSON.parse(text) : {};
  if (!response.ok) {
    throw new Error(body.message || body.status || "Request failed");
  }
  return body;
}

async function requestText(url, options = {}) {
  const response = await fetch(url, options);
  const text = await response.text();
  if (!response.ok) {
    throw new Error(text || "Request failed");
  }
  return text;
}

async function sendCommand(command) {
  const useModal = usesBlockingCommandModal(command);
  if (state.commandModalOpen) {
    return null;
  }
  const presentation = commandPresentation(command);
  const waitType = commandWaitType(command);
  const startSeq = state.data?.filterCycles?.seq ?? 0;
  if (useModal) {
    setCommandModal("pending", presentation.title, presentation.details);
  }
  console.log("[bp100g2] send command", command);
  try {
    const result = await requestJson("/api/cmd", {
      method: "POST",
      body: JSON.stringify(command),
    });
    console.log("[bp100g2] command result", result);
    if (useModal && waitType) {
      await waitForCommandCompletion(waitType, command, startSeq);
    } else if (useModal) {
      completeCommandModal();
      await new Promise((resolve) => setTimeout(resolve, 1600));
    } else if (!QuietActions.has(command.action)) {
      showToast(result.message || result.status);
    }
    return result;
  } catch (error) {
    console.error("[bp100g2] command failed", command, error);
    if (useModal) {
      failCommandModal(error.message);
    } else {
      showToast(error.message);
    }
    return null;
  }
}

async function refreshStatusCapture() {
  const log = await requestText("/api/status-capture");
  const captureLog = $("#captureLog");
  const wasAtBottom = captureLog.scrollTop + captureLog.clientHeight >= captureLog.scrollHeight - 24;
  captureLog.value = log;
  $("#captureCount").textContent = `${log ? log.trimEnd().split("\n").length : 0} lines`;
  if (wasAtBottom || state.data?.statusCaptureActive) {
    captureLog.scrollTop = captureLog.scrollHeight;
  }
}

function startCapturePolling() {
  if (state.capturePollTimer) return;
  refreshStatusCapture().catch((error) => showToast(error.message));
  state.capturePollTimer = setInterval(() => {
    refreshStatusCapture().catch((error) => showToast(error.message));
  }, 1000);
}

function stopCapturePolling() {
  if (!state.capturePollTimer) return;
  clearInterval(state.capturePollTimer);
  state.capturePollTimer = null;
  refreshStatusCapture().catch((error) => showToast(error.message));
}

// The controller sends state when it changes plus a heartbeat every 5 s, so silence this long means the
// connection is dead even if the browser hasn't noticed.
const WsStaleMs = 12000;
// A drop that's back within this long reconnects without flashing "Reconnecting".
const WsQuietReconnectMs = 3000;

function connectWs() {
  const protocol = location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${protocol}://${location.host}/ws`);
  state.ws = ws;
  let lastMessageAt = Date.now();

  const drop = () => {
    if (state.ws !== ws) return;  // already replaced
    state.ws = null;
    clearInterval(watchdog);
    ws.close();
    state.wsFailures = (state.wsFailures || 0) + 1;
    if (!state.wsStatusTimer) {
      state.wsStatusTimer = setTimeout(() => setConnection("Reconnecting"), WsQuietReconnectMs);
    }
    // Try again almost at once, then back off to 5 s while the link stays down.
    setTimeout(connectWs, Math.min(5000, 250 * 2 ** Math.min(state.wsFailures - 1, 5)));
  };
  const watchdog = setInterval(() => {
    if (Date.now() - lastMessageAt > WsStaleMs) drop();
  }, 2000);

  ws.addEventListener("open", () => {
    state.wsFailures = 0;
    clearTimeout(state.wsStatusTimer);
    state.wsStatusTimer = null;
    setConnection("Live", true);
  });
  ws.addEventListener("close", drop);
  ws.addEventListener("message", (event) => {
    lastMessageAt = Date.now();
    const message = JSON.parse(event.data);
    if (message.type === "state") {
      renderState(message.data);
    }
    if (message.type === "cmd_result") {
      showToast(message.message || message.status);
    }
  });
}

async function loadConfig() {
  const message = await requestJson("/api/config");
  applyConfig(message.data);
}

function applyConfig(config) {
  state.config = config;
  const form = $("#settingsForm");
  field(form, "timezone").value = state.config.timezone || "";
  syncTimezonePreset();
  field(form, "authEnabled").checked = !!state.config.authEnabled;
  const caps = state.config.capabilities || {};
  syncDependentFields();
  ["setTime"].forEach((key) => {
    field(form, key).checked = !!caps[key];
  });
  field(form, "showJet2").checked = !caps.hideJet2;
  field(form, "latitude").value = state.config.location ? state.config.location.lat : "";
  field(form, "longitude").value = state.config.location ? state.config.location.lon : "";
  fillEnergySettings(config);
  refreshWeatherForLocation();
}

// Runtime may have loaded before the location was known (e.g. a refresh on that tab); catch the
// outside temperature up now. loadWeather is cached per location, so this is free when it's current.
function refreshWeatherForLocation() {
  if (state.runtimeDays) {
    loadWeather()
      .then(renderRuntime)
      .catch((error) => console.warn("[bp100g2] weather failed", error));
  }
}

function syncDependentFields() {
  const form = $("#settingsForm");
  $("#authPasswordRow").hidden = !field(form, "authEnabled").checked;
}

// "/2" (2 AM changeover) is the POSIX default, so rules with and without it are the same zone.
function sameTimezone(a, b) {
  return a.replace(/\/2(?=,|$)/g, "") === b.replace(/\/2(?=,|$)/g, "");
}

function syncTimezonePreset() {
  const rule = field($("#settingsForm"), "timezone").value.trim();
  const preset = $$("#timezonePreset option").find((option) => option.value !== "custom" && sameTimezone(option.value, rule));
  $("#timezonePreset").value = preset ? preset.value : "custom";
  $("#timezoneCustomRow").hidden = !!preset;
}

function showView(id) {
  const view = $(`#${id}`);
  if (!view?.classList.contains("view")) return;
  $$(".nav-btn").forEach((tab) => tab.classList.toggle("active", tab.dataset.view === id));
  $$(".view").forEach((other) => other.classList.toggle("active", other === view));
  try {
    sessionStorage.setItem("view", id);  // a refresh comes back here; a fresh launch starts on Home
  } catch (error) {
    // Storage can be unavailable (private mode); the page just won't be remembered.
  }
  const activeSubtab = view.querySelector(".subtab.active");
  if (activeSubtab) onSubtabShown(activeSubtab.dataset.subtab);
  if (id === "maintenance") {
    loadMaintenance().catch((error) => showToast(error.message));
    loadWater().catch((error) => showToast(error.message));
  }
}

// ── maintenance ──

const FilterCleanDueDays = 28;
const MaintHistoryPreview = 8;

// Seconds on the controller's clock, so in-tub time doesn't depend on the phone's clock being right.
function controllerNow() {
  return Math.floor(Date.now() / 1000 + state.maintClockOffset);
}

async function loadMaintenance() {
  const data = await requestJson("/api/maintenance");
  setMaintenance(data);
}

function setMaintenance(data) {
  state.maintenance = data;
  state.maintClockOffset = (data.now || Date.now() / 1000) - Date.now() / 1000;
  state.maintLoadedAt = Date.now();
  renderMaintenance();
}

function tubSeconds(filter, since = 0) {
  const now = controllerNow();
  return (filter.stints || []).reduce((total, [start, end]) => {
    const from = Math.max(start, since);
    const to = Math.min(end || now, now);
    return total + Math.max(0, to - from);
  }, 0);
}

function daysText(seconds) {
  const days = seconds / 86400;
  if (days < 1) return `${Math.max(0, Math.round(seconds / 3600))} h`;
  const whole = Math.floor(days);
  return `${whole} ${whole === 1 ? "day" : "days"}`;
}

function pumpedText(seconds) {
  return seconds < 3600 ? `${Math.round(seconds / 60)} min` : `${Math.round(seconds / 3600)} h`;
}

function dateText(ts, withTime = false) {
  const options = { month: "short", day: "numeric" };
  if (new Date(ts * 1000).getFullYear() !== new Date().getFullYear()) options.year = "numeric";
  if (withTime) Object.assign(options, { hour: "numeric", minute: "2-digit" });
  return new Date(ts * 1000).toLocaleString(undefined, options);
}

function maintRow(label, value, className = "") {
  const row = document.createElement("div");
  row.className = "form-row";
  const name = document.createElement("span");
  name.className = "form-row-label";
  name.textContent = label;
  const text = document.createElement("strong");
  text.className = `form-row-value ${className}`.trim();
  text.textContent = value;
  row.append(name, text);
  return row;
}

function maintButton(label, onClick, className = "") {
  const button = document.createElement("button");
  button.type = "button";
  button.className = `row-btn ${className}`.trim();
  button.textContent = label;
  button.addEventListener("click", onClick);
  return button;
}

// Due state for a recurring task, relative to the controller's clock.
function taskStatus(task) {
  if (!task.lastDone) {
    return { rank: 3, text: "Not done yet", className: "", remaining: Infinity };
  }
  const remaining = task.lastDone + task.intervalDays * 86400 - controllerNow();
  const days = Math.ceil(Math.abs(remaining) / 86400);
  if (remaining < 0) {
    return { rank: 0, text: `Overdue by ${days} ${days === 1 ? "day" : "days"}`, className: "overdue", remaining };
  }
  if (remaining < 86400) return { rank: 1, text: "Due today", className: "soon", remaining };
  if (remaining <= 7 * 86400) return { rank: 1, text: `Due in ${days} days`, className: "soon", remaining };
  return { rank: 2, text: `In ${days} days`, className: "", remaining };
}

function filterCleanDue(filter) {
  return tubSeconds(filter, filter.cleanedAt || filter.createdAt) >= FilterCleanDueDays * 86400;
}

function renderTasks(tasks) {
  const rows = tasks
    .map((task) => ({ task, status: taskStatus(task) }))
    .sort((a, b) => a.status.rank - b.status.rank || a.status.remaining - b.status.remaining)
    .map(({ task, status }) => {
      const row = document.createElement("div");
      row.className = "task-row";
      const info = document.createElement("button");
      info.type = "button";
      info.className = "task-info";
      info.setAttribute("aria-label", `Edit ${task.name}`);
      const name = document.createElement("span");
      name.className = "task-name";
      name.textContent = task.name;
      const meta = document.createElement("span");
      meta.className = "task-meta";
      meta.textContent = `Every ${task.intervalDays} ${task.intervalDays === 1 ? "day" : "days"}` +
        (task.lastDone ? ` · last ${dateText(task.lastDone)}` : "");
      info.append(name, meta);
      info.addEventListener("click", () => openMaintSheet({ action: "editTask", task }));
      const statusText = document.createElement("span");
      statusText.className = `task-status ${status.className}`.trim();
      statusText.textContent = status.text;
      row.append(info, statusText, maintButton("Done", () => openMaintSheet({ action: "taskDone", task }), "narrow"));
      return row;
    });
  if (!rows.length) {
    const empty = document.createElement("div");
    empty.className = "maint-empty";
    empty.textContent = "No tasks. Add chores like draining and refilling the tub.";
    rows.push(empty);
  }
  $("#taskList").replaceChildren(...rows);
}

const EventText = {
  added: (name) => `Added ${name}`,
  installed: (name) => `${name} went in`,
  removed: (name) => `${name} came out`,
  cleaned: (name) => `${name} cleaned`,
  deleted: (name) => `Deleted ${name}`,
  done: (name) => `${name} done`,
};

function renderMaintenance() {
  const data = state.maintenance;
  if (!data) return;
  const filters = data.filters || [];
  const tasks = data.tasks || [];
  renderTasks(tasks);
  const tasksOverdue = tasks.some((task) => taskStatus(task).rank === 0);
  const filtersDue = filters.some((filter) => filter.inTub && filterCleanDue(filter));
  Object.assign(state.maintAlerts, { tasks: tasksOverdue, filters: filtersDue });
  updateMaintNavAlert();
  $('.subtab[data-subtab="maintTasks"]').classList.toggle("has-alert", tasksOverdue);
  $('.subtab[data-subtab="maintFilters"]').classList.toggle("has-alert", filtersDue);
  const inTub = filters.filter((filter) => filter.inTub);
  const cards = filters.map((filter) => {
    const card = document.createElement("div");
    card.className = `maint-card${filter.inTub ? " in-tub" : ""}`;
    const head = document.createElement("div");
    head.className = "maint-card-head";
    const name = document.createElement("strong");
    name.textContent = filter.name;
    const badge = document.createElement("span");
    badge.className = `badge${filter.inTub ? " in" : ""}`;
    badge.textContent = filter.inTub ? "In tub" : "Out";
    head.append(name, badge);
    card.append(head);

    const since = filter.cleanedAt || filter.createdAt;
    const tubSinceClean = tubSeconds(filter, since);
    const due = filterCleanDue(filter);
    card.append(maintRow(filter.cleanedAt ? "Since cleaned" : "Since added",
      `${daysText(tubSinceClean)} in tub · ${pumpedText(filter.pumpSecSinceClean)} pumped${due ? " · clean due" : ""}`,
      due ? "due" : filter.inTub ? "" : "muted"));
    card.append(maintRow("Lifetime", `${daysText(tubSeconds(filter))} in tub · ${pumpedText(filter.pumpSec)} pumped`, "muted"));
    card.append(maintRow("Last cleaned", filter.cleanedAt ? dateText(filter.cleanedAt) : "Never", filter.cleanedAt ? "muted" : ""));

    const actions = document.createElement("div");
    actions.className = "maint-actions";
    if (filter.inTub) {
      actions.append(maintButton("Take Out", () => openMaintSheet({ action: "remove", filter })));
    } else {
      const swapWith = inTub.length === 1 ? inTub[0] : null;
      actions.append(maintButton(swapWith ? "Swap In" : "Put In",
        () => openMaintSheet(swapWith ? { action: "swap", filter, swapWith } : { action: "install", filter }), "accent"));
    }
    actions.append(maintButton("Cleaned", () => openMaintSheet({ action: "clean", filter })));
    actions.append(maintButton("Edit", () => openMaintSheet({ action: "edit", filter }), "narrow"));
    card.append(actions);
    return card;
  });
  if (!cards.length) {
    const empty = document.createElement("div");
    empty.className = "maint-empty";
    empty.textContent = "No filters yet. Add each filter you own, then mark which one is in the tub.";
    cards.push(empty);
  }
  $("#filterCards").replaceChildren(...cards);

  const allEvents = data.events || [];
  renderHistory(allEvents.filter((event) => event.type === "done"), "task");
  renderHistory(allEvents.filter((event) => event.type !== "done"), "filter");
}

function updateMaintNavAlert() {
  const alert = Object.values(state.maintAlerts).some(Boolean);
  $('.nav-btn[data-view="maintenance"]').classList.toggle("has-alert", alert);
}

// Month-grouped history list; `kind` picks the list ("task", "filter" or "water") and its Show all
// toggle. `text` describes an event; events `selectable` accepts can be tapped to call `onSelect`.
function renderHistory(events, kind, { text = (event) => (EventText[event.type] || EventText.added)(event.name),
  selectable = () => false, onSelect = null } = {}) {
  // Newest first by the date it happened, which can differ from the order it was recorded in.
  const sorted = events
    .map((event, index) => ({ ...event, index }))
    .sort((a, b) => b.ts - a.ts || b.index - a.index);
  const expanded = !!state.historyExpanded[kind];
  const shown = expanded ? sorted : sorted.slice(0, MaintHistoryPreview);
  const items = [];
  let month = "";
  shown.forEach((event) => {
    const date = new Date(event.ts * 1000);
    const label = date.toLocaleDateString(undefined, date.getFullYear() === new Date().getFullYear()
      ? { month: "long" } : { month: "long", year: "numeric" });
    if (label !== month) {
      month = label;
      const heading = document.createElement("li");
      heading.className = "month-heading";
      heading.textContent = label;
      items.push(heading);
    }
    const item = document.createElement("li");
    item.textContent = text(event);
    if (selectable(event)) {
      item.className = "tappable";
      item.addEventListener("click", () => onSelect(event));
    }
    const meta = document.createElement("span");
    meta.className = "fault-meta";
    meta.textContent = dateText(event.ts, true);
    item.append(meta);
    items.push(item);
  });
  if (!items.length) {
    const item = document.createElement("li");
    item.className = "empty";
    item.textContent = "Nothing recorded yet";
    items.push(item);
  }
  $(`#${kind}History`).replaceChildren(...items);
  const toggle = $(`#${kind}HistoryToggle`);
  toggle.hidden = sorted.length <= MaintHistoryPreview;
  toggle.textContent = expanded ? "Show less" : `Show all (${sorted.length})`;
}

function localDateTimeValue(date) {
  const pad = (n) => String(n).padStart(2, "0");
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}T${pad(date.getHours())}:${pad(date.getMinutes())}`;
}

function openMaintSheet(sheet) {
  state.maintSheet = sheet;
  const { action, filter, swapWith, task } = sheet;
  const nameActions = ["add", "edit", "addTask", "editTask"];
  const titles = {
    addTask: "Add Task",
    editTask: "Edit Task",
    taskDone: `${task?.name} Done`,
    add: "Add Filter",
    edit: "Edit Filter",
    install: `Put In ${filter?.name}`,
    swap: `Swap In ${filter?.name}`,
    remove: `Take Out ${filter?.name}`,
    clean: `Clean ${filter?.name}`,
  };
  const texts = {
    swap: `${swapWith?.name} comes out and ${filter?.name} goes in.`,
    clean: "Resets the since-cleaned time. Lifetime totals keep counting.",
    remove: "Pumped time stops counting for this filter until it goes back in.",
    taskDone: task ? `Next due ${task.intervalDays} days after this date.` : "",
  };
  $("#maintSheetTitle").textContent = titles[action];
  $("#maintSheetText").textContent = texts[action] || "";
  $("#maintNameRow").hidden = !nameActions.includes(action);
  $("#maintIntervalRow").hidden = !["addTask", "editTask"].includes(action);
  $("#maintDateRow").hidden = nameActions.includes(action);
  $("#maintName").value = action === "edit" ? filter.name : action === "editTask" ? task.name : "";
  $("#maintInterval").value = action === "editTask" ? task.intervalDays : 30;
  $("#maintDate").value = localDateTimeValue(new Date());
  $("#maintDate").max = localDateTimeValue(new Date());
  $("#maintDelete").hidden = !["edit", "editTask"].includes(action);
  $("#maintDelete").textContent = action === "editTask" ? "Delete Task" : "Delete Filter";
  delete $("#maintDelete").dataset.armed;
  $("#maintConfirm").textContent = ["add", "addTask"].includes(action) ? "Add"
    : ["edit", "editTask"].includes(action) ? "Save" : "Confirm";
  $("#maintSheet").classList.remove("hidden");
  if (!$("#maintNameRow").hidden) $("#maintName").focus();
}

function closeMaintSheet() {
  state.maintSheet = null;
  $("#maintSheet").classList.add("hidden");
}

async function submitMaintenance(body) {
  try {
    const data = await requestJson("/api/maintenance", { method: "POST", body: JSON.stringify(body) });
    setMaintenance(data);
    closeMaintSheet();
  } catch (error) {
    showToast(error.message);
  }
}

function confirmMaintSheet() {
  const sheet = state.maintSheet;
  if (!sheet) return;
  const ts = Math.floor(new Date($("#maintDate").value).getTime() / 1000);
  if (!["add", "edit", "addTask", "editTask"].includes(sheet.action) && !Number.isFinite(ts)) {
    showToast("Pick a date and time");
    return;
  }
  const name = $("#maintName").value.trim();
  const intervalDays = Number($("#maintInterval").value);
  switch (sheet.action) {
    case "addTask":
      submitMaintenance({ action: "addTask", name, intervalDays });
      break;
    case "editTask":
      submitMaintenance({ action: "editTask", id: sheet.task.id, name, intervalDays });
      break;
    case "taskDone":
      submitMaintenance({ action: "taskDone", id: sheet.task.id, ts });
      break;
    case "add":
      submitMaintenance({ action: "add", name });
      break;
    case "edit":
      submitMaintenance({ action: "rename", id: sheet.filter.id, name });
      break;
    case "swap":
      submitMaintenance({ action: "swap", outId: sheet.swapWith.id, inId: sheet.filter.id, ts });
      break;
    default:
      submitMaintenance({ action: sheet.action, id: sheet.filter.id, ts });
  }
}

function setupMaintenance() {
  $("#addFilter").addEventListener("click", () => openMaintSheet({ action: "add" }));
  $("#addTask").addEventListener("click", () => openMaintSheet({ action: "addTask" }));
  ["task", "filter"].forEach((kind) => {
    $(`#${kind}HistoryToggle`).addEventListener("click", () => {
      state.historyExpanded[kind] = !state.historyExpanded[kind];
      renderMaintenance();
    });
  });
  $("#maintCancel").addEventListener("click", closeMaintSheet);
  $("#maintConfirm").addEventListener("click", confirmMaintSheet);
  $("#maintName").addEventListener("keydown", (event) => {
    if (event.key === "Enter") confirmMaintSheet();
  });
  $("#maintSheet").addEventListener("click", (event) => {
    if (event.target.id === "maintSheet") closeMaintSheet();
  });
  $("#maintDelete").addEventListener("click", (event) => {
    const button = event.currentTarget;
    if (!button.dataset.armed) {
      button.dataset.armed = "1";  // second tap deletes, so a stray tap can't
      button.textContent = "Tap again to delete";
      return;
    }
    const sheet = state.maintSheet;
    submitMaintenance(sheet.task ? { action: "deleteTask", id: sheet.task.id } : { action: "delete", id: sheet.filter.id });
  });
}

// ── water care ──

const SanitizerTargets = {
  frog: { name: "Free Chlorine", short: "Cl", ok: [0.5, 1], near: [0.25, 3] },
  chlorine: { name: "Free Chlorine", short: "Cl", ok: [3, 5], near: [1, 8] },
  bromine: { name: "Bromine", short: "Br", ok: [3, 5], near: [2, 8] },
};

const PadTargets = {
  ph: { name: "pH", short: "pH", unit: "", ok: [7.2, 7.8], near: [7, 8] },
  ta: { name: "Alkalinity", short: "TA", unit: " ppm", ok: [80, 120], near: [60, 180] },
  th: { name: "Hardness", short: "TH", unit: " ppm", ok: [100, 250], near: [50, 400] },
};

const CartridgeNames = { sanitizer: "SmartChlor", mineral: "Mineral" };
const DefaultChemicals = ["pH Up", "pH Down", "Alkalinity Up", "Alkalinity Down", "Shock (non-chlorine)",
  "Chlorine shock", "Chlorine", "Bromine", "FROG Jump Start", "Calcium Increaser", "Defoamer"];
const TestStaleDays = 7;

function dialText(dial) {
  return dial === 0 ? "0 (closed)" : String(dial);
}

function waterSystem() {
  return state.water?.system || "frog";
}

function padTargets() {
  return { san: { ...SanitizerTargets[waterSystem()], unit: " ppm" }, ...PadTargets };
}

function numberText(value) {
  return String(Math.round(value * 100) / 100);
}

// pH always reads with one decimal, like the strip's color chart.
function padText(key, value) {
  return key === "ph" ? value.toFixed(1) : numberText(value);
}

// FROG @ease strips are read by which color zone the pad lands in, not in ppm.
const SanLevelText = { 1: "Low", 2: "OK", 3: "High" };

// Like padStatus, for a sanitizer pad that may have been read by color zone instead.
function sanStatus(pad, target) {
  if (!pad.level) return padStatus(pad.value, target);
  return { level: pad.level === 2 ? "ok" : "near", dir: { 1: "low", 3: "high" }[pad.level] || null };
}

function sanText(pad, target) {
  return pad.level ? `${SanLevelText[pad.level]} zone` : `${padText("san", pad.value)}${target.unit}`;
}

function rangeText([low, high]) {
  return `${numberText(low)}–${numberText(high)}`;
}

// "ok" inside the target, "near" just outside it, "far" beyond; with which way it's off.
function padStatus(value, target) {
  const dir = value < target.ok[0] ? "low" : value > target.ok[1] ? "high" : null;
  if (!dir) return { level: "ok", dir };
  const near = value >= target.near[0] && value <= target.near[1];
  return { level: near ? "near" : "far", dir };
}

async function loadWater() {
  setWater(await requestJson("/api/water"));
}

function setWater(data) {
  state.water = data;
  state.maintClockOffset = (data.now || Date.now() / 1000) - Date.now() / 1000;
  renderWater();
}

function readings() {
  return (state.water?.entries || []).filter((entry) => entry.type === "reading").sort((a, b) => a.ts - b.ts);
}

// Most recent value of each pad; hardness is often read less often than the rest.
function latestPads() {
  const pads = {};
  readings().forEach((entry) => {
    Object.keys(PadTargets).concat("san").forEach((key) => {
      if (Number.isFinite(entry[key])) pads[key] = { value: entry[key], ts: entry.ts };
    });
    if (entry.sanLevel) pads.san = { level: entry.sanLevel, ts: entry.ts };
  });
  return pads;
}

function waterAdvice(pads) {
  const targets = padTargets();
  const off = (key) => (!pads[key] ? null
    : key === "san" ? sanStatus(pads.san, targets.san).dir : padStatus(pads[key].value, targets[key]).dir);
  const frog = waterSystem() === "frog";
  const sanName = targets.san.name.toLowerCase();
  const tips = [];
  if (off("ta") === "low") tips.push("Alkalinity is low. Raise it with Alkalinity Up first; it holds pH steady.");
  if (off("ta") === "high") tips.push("Alkalinity is high. Lower it with pH Down a little at a time, then retest.");
  if (!off("ta")) {
    if (off("ph") === "low") tips.push("pH is low: add pH Up.");
    if (off("ph") === "high") tips.push("pH is high: add pH Down.");
  } else if (off("ph")) {
    tips.push("Fix alkalinity before chasing pH.");
  }
  if (off("san") === "low") {
    tips.push(frog ? "Chlorine is low: turn the SmartChlor dial up one, or replace the cartridge if it's empty."
      : `${targets.san.name} is low: add ${sanName} or shock.`);
  }
  if (off("san") === "high") {
    tips.push(frog ? "Chlorine is high: turn the SmartChlor dial down one."
      : `${targets.san.name} is high: let it drop before soaking.`);
  }
  if (off("th") === "low") tips.push("Hardness is low; soft water can foam and corrode. Calcium Increaser raises it.");
  if (off("th") === "high") tips.push("Hardness is high and can scale the heater. A drain and refill brings it down.");
  return tips;
}

function renderLatestTest(pads) {
  const card = $("#latestTest");
  const targets = padTargets();
  const latest = readings().at(-1);
  const head = document.createElement("div");
  head.className = "maint-card-head";
  const title = document.createElement("strong");
  title.textContent = latest ? `Tested ${dateText(latest.ts)}` : "No tests yet";
  head.append(title);
  if (latest) {
    const age = controllerNow() - latest.ts;
    const badge = document.createElement("span");
    const stale = age > TestStaleDays * 86400;
    badge.className = `badge${stale ? " warn" : ""}`;
    badge.textContent = age < 86400 ? "Today" : daysText(age) + " ago";
    head.append(badge);
  }
  const rows = ["san", "ph", "ta", "th"].filter((key) => pads[key]).map((key) => {
    const target = targets[key];
    const { value, ts, level } = pads[key];
    const status = key === "san" ? sanStatus(pads.san, target) : padStatus(value, target);
    const label = level ? target.name : `${target.name} · ${rangeText(target.ok)}`;
    const word = status.dir && !level ? ` · ${status.dir === "low" ? "Low" : "High"}` : "";
    const older = ts !== latest.ts ? ` (${dateText(ts)})` : "";
    const className = status.level === "far" ? "bad" : status.level === "near" ? "due" : "";
    const text = key === "san" ? sanText(pads.san, target) : `${padText(key, value)}${target.unit}`;
    return maintRow(label, `${text}${word}${older}`, className);
  });
  if (!latest) {
    rows.push(maintRow("Dip a strip and log what it reads.", "", "muted"));
  }
  const actions = document.createElement("div");
  actions.className = "maint-actions";
  actions.append(maintButton("Log Test", () => openWaterSheet({ action: "reading" }), "accent"));
  actions.append(maintButton("Add Chemical", () => openWaterSheet({ action: "dose" })));
  card.replaceChildren(head, ...rows, actions);

  const tips = waterAdvice(pads).map((tip) => {
    const item = document.createElement("li");
    item.textContent = tip;
    return item;
  });
  $("#waterAdvice").replaceChildren(...tips);
}

function cartridgeStatus(cartridge) {
  return taskStatus({ lastDone: cartridge.installedAt, intervalDays: cartridge.lifeDays });
}

function renderCartridges() {
  const cartridges = state.water?.cartridges || {};
  const cards = Object.entries(CartridgeNames).map(([slot, name]) => {
    const cartridge = cartridges[slot] || {};
    const installed = !!cartridge.installedAt;
    const card = document.createElement("div");
    card.className = `maint-card${installed ? " in-tub" : ""}`;
    const head = document.createElement("div");
    head.className = "maint-card-head";
    const title = document.createElement("strong");
    title.textContent = name;
    const badge = document.createElement("span");
    badge.className = `badge${installed ? " in" : ""}`;
    badge.textContent = installed ? "In tub" : "Not recorded";
    head.append(title, badge);
    card.append(head);

    const actions = document.createElement("div");
    actions.className = "maint-actions";
    if (installed) {
      const status = cartridgeStatus(cartridge);
      card.append(maintRow("Age", `${daysText(controllerNow() - cartridge.installedAt)} · ${pumpedText(cartridge.pumpSec)} pumped`));
      card.append(maintRow(`Replace · every ${cartridge.lifeDays} days`, status.text,
        status.rank === 0 ? "due" : status.rank === 1 ? "" : "muted"));
      if (slot === "sanitizer") {
        const known = Number.isFinite(cartridge.dial);
        card.append(maintRow("Dial", known ? dialText(cartridge.dial) : "Not set", known ? "" : "muted"));
      }
      actions.append(maintButton("Replaced", () => openWaterSheet({ action: "replace", slot }), "accent"));
      if (slot === "sanitizer") actions.append(maintButton("Dial", () => openWaterSheet({ action: "dial", slot })));
    } else {
      card.append(maintRow("Record when it went in to track its life", "", "muted"));
      actions.append(maintButton("Put In", () => openWaterSheet({ action: "replace", slot }), "accent"));
    }
    actions.append(maintButton("Edit", () => openWaterSheet({ action: "life", slot }), "narrow"));
    card.append(actions);
    return card;
  });
  $("#cartridgeCards").replaceChildren(...cards);
}

function waterEntryText(entry) {
  const note = entry.note ? ` — ${entry.note}` : "";
  switch (entry.type) {
    case "reading": {
      const targets = padTargets();
      const pads = ["san", "ph", "ta", "th"].filter((key) => Number.isFinite(entry[key]))
        .map((key) => `${targets[key].short} ${padText(key, entry[key])}`);
      if (entry.sanLevel) pads.unshift(`${targets.san.short} ${SanLevelText[entry.sanLevel]}`);
      return `Test · ${pads.join(" · ")}${note}`;
    }
    case "dose":
      return `${entry.chem} · ${numberText(entry.amount)} ${entry.unit}${note}`;
    case "cartridge":
      return entry.slot === "mineral" ? "Mineral cartridge replaced"
        : `SmartChlor replaced${Number.isFinite(entry.dial) ? ` · dial ${dialText(entry.dial)}` : ""}`;
    case "dial":
      return `SmartChlor dial set to ${dialText(entry.dial)}`;
    default:
      return "";
  }
}

function renderWater() {
  const data = state.water;
  if (!data) return;
  const frog = waterSystem() === "frog";
  $("#waterSystem").value = waterSystem();
  $("#cartridgeSection").hidden = !frog;
  renderLatestTest(latestPads());
  if (frog) renderCartridges();

  const cartridgeDue = frog && Object.values(data.cartridges || {})
    .some((cartridge) => cartridge.installedAt && cartridgeStatus(cartridge).rank === 0);
  state.maintAlerts.water = cartridgeDue;
  updateMaintNavAlert();
  $('.subtab[data-subtab="maintWater"]').classList.toggle("has-alert", cartridgeDue);

  const targets = padTargets();
  $("#waterTargets").textContent = `Targets: ${["san", "ph", "ta", "th"]
    .map((key) => (key === "san" && frog ? `${targets.san.name} in the strip's OK zone`
      : `${targets[key].name} ${rangeText(targets[key].ok)}${targets[key].unit}`))
    .join(" · ")}.${frog ? " FROG @ease runs low chlorine; the SmartChlor dial sets how much." : ""}`;

  renderHistory(data.entries || [], "water", {
    text: waterEntryText,
    selectable: (entry) => entry.type === "reading" || entry.type === "dose",
    onSelect: (entry) => openWaterSheet({ action: "entry", entry }),
  });

  const chemicals = new Set(DefaultChemicals);
  (data.entries || []).filter((entry) => entry.type === "dose").forEach((entry) => chemicals.add(entry.chem));
  $("#chemicalList").replaceChildren(...[...chemicals].map((chem) => {
    const option = document.createElement("option");
    option.value = chem;
    return option;
  }));
}

function openWaterSheet(sheet) {
  state.waterSheet = sheet;
  const { action, slot, entry } = sheet;
  const cartridge = state.water?.cartridges?.[slot] || {};
  const name = CartridgeNames[slot];
  const titles = {
    reading: "Log Test",
    dose: "Add Chemical",
    replace: cartridge.installedAt ? `Replace ${name}` : `Put In ${name}`,
    dial: "SmartChlor Dial",
    life: `${name} Cartridge`,
    entry: entry ? waterEntryText({ ...entry, note: "" }) : "",
  };
  const texts = {
    reading: "Leave a pad blank if you didn't read it.",
    replace: "Day and pumped counts start over.",
    entry: entry ? `${dateText(entry.ts, true)}${entry.note ? ` · ${entry.note}` : ""}` : "",
  };
  $("#waterSheetTitle").textContent = titles[action];
  $("#waterSheetText").textContent = texts[action] || "";
  $$("#waterFields .form-row").forEach((row) => {
    row.hidden = !row.dataset.for.split(" ").includes(action) ||
      (row.contains($("#wDial")) && action === "replace" && slot !== "sanitizer");
  });
  $("#waterFields").hidden = action === "entry";
  $("#wSanLabel").textContent = SanitizerTargets[waterSystem()].name;
  if (action === "reading") {
    const frog = waterSystem() === "frog";
    $("#wSanRow").hidden = frog;
    $("#wSanLevelRow").hidden = !frog;
  }
  ["wSan", "wSanLevel", "wPh", "wTa", "wTh", "wChem", "wAmount", "wNote"].forEach((id) => { $(`#${id}`).value = ""; });
  $("#wAmount").placeholder = "";
  $("#wDial").value = String(state.water?.cartridges?.sanitizer?.dial ?? 3);
  $("#wLife").value = cartridge.lifeDays || 30;
  $("#wDate").value = localDateTimeValue(new Date());
  $("#wDate").max = localDateTimeValue(new Date());
  $("#waterDelete").hidden = action !== "entry";
  delete $("#waterDelete").dataset.armed;
  $("#waterDelete").textContent = "Delete Entry";
  $("#waterConfirm").hidden = action === "entry";
  $("#waterConfirm").textContent = action === "reading" || action === "dose" ? "Add" : "Save";
  $("#waterCancel").textContent = action === "entry" ? "Close" : "Cancel";
  $("#waterSheet").classList.remove("hidden");
}

function closeWaterSheet() {
  state.waterSheet = null;
  $("#waterSheet").classList.add("hidden");
}

async function submitWater(body) {
  try {
    setWater(await requestJson("/api/water", { method: "POST", body: JSON.stringify(body) }));
    closeWaterSheet();
  } catch (error) {
    showToast(error.message);
  }
}

// The last amount and unit used for a chemical, so a repeat dose is quick to log.
function suggestDose() {
  const chem = $("#wChem").value.trim().toLowerCase();
  const last = (state.water?.entries || []).filter((entry) => entry.type === "dose" && entry.chem.toLowerCase() === chem).at(-1);
  if (!last) return;
  $("#wUnit").value = last.unit;
  $("#wAmount").placeholder = numberText(last.amount);
}

function confirmWaterSheet() {
  const sheet = state.waterSheet;
  if (!sheet) return;
  const ts = Math.floor(new Date($("#wDate").value).getTime() / 1000);
  if (["reading", "dose", "replace", "dial"].includes(sheet.action) && !Number.isFinite(ts)) {
    showToast("Pick a date and time");
    return;
  }
  const number = (id) => ($(`#${id}`).value.trim() === "" ? null : Number($(`#${id}`).value));
  const note = $("#wNote").value.trim();
  switch (sheet.action) {
    case "reading": {
      const frog = waterSystem() === "frog";
      const pads = { san: frog ? null : number("wSan"), sanLevel: frog ? number("wSanLevel") : null,
        ph: number("wPh"), ta: number("wTa"), th: number("wTh") };
      Object.keys(pads).forEach((key) => { if (!Number.isFinite(pads[key])) delete pads[key]; });
      if (!Object.keys(pads).length) {
        showToast("Enter at least one reading");
        return;
      }
      submitWater({ action: "reading", ts, ...pads, note });
      break;
    }
    case "dose": {
      const amount = number("wAmount") ?? (Number($("#wAmount").placeholder) || null);
      submitWater({ action: "dose", ts, chem: $("#wChem").value.trim(), amount, unit: $("#wUnit").value, note });
      break;
    }
    case "replace":
      submitWater({ action: "replace", slot: sheet.slot, ts, ...(sheet.slot === "sanitizer" ? { dial: Number($("#wDial").value) } : {}) });
      break;
    case "dial":
      submitWater({ action: "dial", ts, dial: Number($("#wDial").value) });
      break;
    case "life":
      submitWater({ action: "life", slot: sheet.slot, days: Number($("#wLife").value) });
      break;
    default:
      break;
  }
}

function setupWater() {
  $("#waterHistoryToggle").addEventListener("click", () => {
    state.historyExpanded.water = !state.historyExpanded.water;
    renderWater();
  });
  $("#waterSystem").addEventListener("change", (event) => submitWater({ action: "system", system: event.target.value }));
  $("#wChem").addEventListener("change", suggestDose);
  $("#waterCancel").addEventListener("click", closeWaterSheet);
  $("#waterConfirm").addEventListener("click", confirmWaterSheet);
  $("#waterSheet").addEventListener("click", (event) => {
    if (event.target.id === "waterSheet") closeWaterSheet();
  });
  $("#waterDelete").addEventListener("click", (event) => {
    const button = event.currentTarget;
    if (!button.dataset.armed) {
      button.dataset.armed = "1";  // second tap deletes, so a stray tap can't
      button.textContent = "Tap again to delete";
      return;
    }
    submitWater({ action: "delete", id: state.waterSheet.entry.id });
  });
}

// Sub-tabs are grouped by their .subtabs bar; switching one group leaves the others alone.
function showSubtab(id) {
  const group = $(`.subtab[data-subtab="${id}"]`)?.closest(".subtabs");
  if (!group) return;
  group.querySelectorAll(".subtab").forEach((tab) => {
    const active = tab.dataset.subtab === id;
    tab.classList.toggle("active", active);
    tab.setAttribute("aria-selected", active ? "true" : "false");
    $(`#${tab.dataset.subtab}`).classList.toggle("active", active);
  });
  try {
    sessionStorage.setItem(`subtab:${group.closest(".view").id}`, id);
  } catch (error) {
    // Storage can be unavailable (private mode); the tab just won't be remembered.
  }
  if (group.closest(".view").classList.contains("active")) {
    onSubtabShown(id);
  }
}

function onSubtabShown(id) {
  if (id === "statsOverview") {
    loadRuntime().catch((error) => console.warn("[bp100g2] runtime failed", error));
    if (!state.faultLog) loadFaults().catch((error) => console.warn("[bp100g2] faults failed", error));
  } else if (id === "statsRuntime") {
    loadRuntime().catch((error) => showToast(error.message));
  } else if (id === "statsDiagnostics" && !state.faultLogRequested) {
    readFaultLog();  // first visit: ask the spa for its log
  }
}

function readFaultLog() {
  state.faultLogRequested = true;
  $("#faultLogState").textContent = "Reading from spa…";
  sendCommand({ action: "readFaultLog" });
}

// ── filter cycle settings ──

function cycleDurationText(minutes) {
  const hours = Math.floor(minutes / 60);
  const rest = minutes % 60;
  if (!hours) return `${rest} m`;
  return rest ? `${hours} h ${rest} m` : `${hours} h`;
}

function renderFilterSettings(data) {
  const filters = data.filterCycles || {};
  if (!filters.valid) {
    ["filter1Summary", "filter2Summary"].forEach((id) => setValue(id, "Waiting for the spa…"));
    $("#timelineTrack").replaceChildren();
    return;
  }
  const cycles = filterCycles(filters);
  cycles.forEach((cycle) => {
    $(`#filter${cycle.n}Summary`).textContent = cycle.enabled
      ? `${rangeLabel(cycle.start, cycle.length)} · ${cycleDurationText(cycle.length)}`
      : "Off";
  });
  const toggle = $("#filter2Toggle");
  if (!state.commandModalOpen) toggle.checked = !!filters.cycle2Enabled;

  // 24-hour bar; a cycle that runs past midnight is drawn as two pieces.
  const now = spaMinutes(data.spaTime);
  const pieces = [];
  cycles.filter((cycle) => cycle.enabled).forEach((cycle) => {
    const running = now !== null && (now - cycle.start + 1440) % 1440 < cycle.length;
    const end = cycle.start + Math.min(cycle.length, 1440);
    pieces.push({ n: cycle.n, from: cycle.start, to: Math.min(end, 1440), running });
    if (end > 1440) pieces.push({ n: cycle.n, from: 0, to: end - 1440, running });
  });
  const nodes = pieces.map((piece) => {
    const segment = document.createElement("div");
    segment.className = `timeline-segment${piece.running ? " running" : ""}`;
    segment.style.left = `${(piece.from / 1440) * 100}%`;
    segment.style.width = `${((piece.to - piece.from) / 1440) * 100}%`;
    segment.textContent = piece.to - piece.from >= 60 ? piece.n : "";
    return segment;
  });
  if (now !== null) {
    const marker = document.createElement("div");
    marker.className = "timeline-now";
    marker.style.left = `${(now / 1440) * 100}%`;
    nodes.push(marker);
  }
  $("#timelineTrack").replaceChildren(...nodes);
}

function filterSheetValues() {
  const durationHour = Number($("#fsDurHour").value);
  return {
    startHour: toHour24(Number($("#fsStartHour").value), $("#fsStartPeriod").value),
    startMinute: Number($("#fsStartMinute").value),
    durationHour,
    durationMinute: durationHour === 24 ? 0 : Number($("#fsDurMinute").value),
  };
}

function updateFilterSheetEnd() {
  const values = filterSheetValues();
  const length = values.durationHour * 60 + values.durationMinute;
  const end = (values.startHour * 60 + values.startMinute + length) % 1440;
  $("#fsDurMinute").disabled = values.durationHour === 24;
  $("#fsEnd").textContent = length > 0 ? clock12Label(Math.floor(end / 60), end % 60) : "--";
}

function openFilterSheet(n) {
  const filters = state.data?.filterCycles || {};
  if (!filters.valid) {
    showToast("Waiting for the spa to report its filter cycles");
    return;
  }
  state.filterSheet = n;
  const start = toClock12(filters[`cycle${n}Start`] ?? 0);
  $("#filterSheetTitle").textContent = `Filter ${n}`;
  $("#fsStartHour").value = start.hour;
  $("#fsStartPeriod").value = start.period;
  $("#fsStartMinute").value = filters[`cycle${n}StartMinute`] ?? 0;
  // A disabled filter 2 reports no run time; suggest the usual 2 hours.
  const duration = filters[`cycle${n}Duration`] ?? 0;
  const durationMinute = filters[`cycle${n}DurationMinute`] ?? 0;
  const empty = !duration && !durationMinute;
  $("#fsDurHour").value = empty ? 2 : duration;
  $("#fsDurMinute").value = empty ? 0 : durationMinute;
  $("#filterSheetSave").textContent = n === 2 && !filters.cycle2Enabled ? "Turn On" : "Save";
  updateFilterSheetEnd();
  $("#filterSheet").classList.remove("hidden");
}

function closeFilterSheet() {
  state.filterSheet = null;
  $("#filterSheet").classList.add("hidden");
}

function saveFilterSheet() {
  const n = state.filterSheet;
  const values = filterSheetValues();
  if (values.durationHour * 60 + values.durationMinute < 15) {
    showToast("Run time must be at least 15 minutes");
    return;
  }
  closeFilterSheet();
  sendCommand({ action: `setFilter${n}`, ...(n === 2 ? { enabled: true } : {}), ...values });
}

const TempRangeSettleMs = 5000;

function renderTempRange(data) {
  const current = data.tempRangeHigh ? "high" : "low";
  // Until the spa reports the switch, keep showing the choice instead of flicking back.
  const pending = state.tempRangePending;
  if (pending && pending.value !== current && Date.now() < pending.until) return;
  state.tempRangePending = null;
  $("#tempRange").value = current;
  const range = data.setTempRange;
  $("#tempRangeHint").textContent = `${data.tempRangeHigh ? "High" : "Low"} allows ${range ? `${range.min}–${range.max}°F` : "its own limits"}` +
    `, set to ${tempLabel(data.setTemp)}. Each range keeps its own set temperature; use Low to save energy while the tub sits unused.`;
}

function setupTempRange() {
  $("#tempRange").addEventListener("change", async (event) => {
    const value = event.target.value;
    state.tempRangePending = { value, until: Date.now() + TempRangeSettleMs };
    const result = await sendCommand({ action: "tempRange", value });
    if (!result) {
      state.tempRangePending = null;
      if (state.data) renderTempRange(state.data);
    }
  });
}

// ── energy cost ──

const MonthNames = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
const RateFields = { rateSummerPeak: "summerPeak", rateSummerOffPeak: "summerOffPeak",
  rateWinterPeak: "winterPeak", rateWinterOffPeak: "winterOffPeak" };
const PowerFields = { powerHeaterKw: "heaterKw", powerPump1LowKw: "pump1LowKw", powerPump1HighKw: "pump1HighKw",
  powerPump2Kw: "pump2Kw", powerStandbyW: "standbyW", powerTubGallons: "tubGallons" };
const WholeNumberPower = ["standbyW", "tubGallons"];

function ratesSet() {
  const rates = state.config?.tou?.rates;
  return !!rates && Object.values(RateFields).some((key) => rates[key] > 0);
}

function moneyText(value) {
  return `$${value.toFixed(2)}`;
}

function costText(energy) {
  return `${moneyText(energy.cost)} · ${energy.kwh.toFixed(1)} kWh`;
}

function isSummer(date, rates) {
  const month = Math.floor(date / 100) % 100;
  const { summerStartMonth: from, summerEndMonth: to } = rates;
  return from <= to ? month >= from && month <= to : month >= from || month <= to;
}

// Estimated energy and cost for one runtime day, split by load and by peak/off-peak. Days recorded
// before peak tracking have no peak fields and count as all off-peak.
function dayEnergy(day) {
  const { tou: { rates }, power } = state.config;
  const summer = isSummer(day.date, rates);
  const peakRate = summer ? rates.summerPeak : rates.winterPeak;
  const offRate = summer ? rates.summerOffPeak : rates.winterOffPeak;
  const loads = {
    heater: [[day.heatingSec, day.heatingPeakSec, power.heaterKw]],
    jets: [[day.pumpLowSec, day.pumpLowPeakSec, power.pump1LowKw], [day.pumpHighSec, day.pumpHighPeakSec, power.pump1HighKw],
      [day.pump2Sec, day.pump2PeakSec, power.pump2Kw]],
    standby: [[day.countedSec, day.countedPeakSec, power.standbyW / 1000]],
  };
  const result = { kwh: 0, cost: 0, peakCost: 0, parts: {} };
  Object.entries(loads).forEach(([name, parts]) => {
    result.parts[name] = 0;
    parts.forEach(([total = 0, peak = 0, kw]) => {
      const peakKwh = (peak / 3600) * kw;
      const offKwh = (Math.max(0, total - peak) / 3600) * kw;
      const cost = peakKwh * peakRate + offKwh * offRate;
      result.kwh += peakKwh + offKwh;
      result.cost += cost;
      result.peakCost += peakKwh * peakRate;
      result.parts[name] += cost;
    });
  });
  return result;
}

function sumEnergy(days) {
  return days.map(dayEnergy).reduce((sum, day) => {
    sum.kwh += day.kwh;
    sum.cost += day.cost;
    sum.peakCost += day.peakCost;
    Object.entries(day.parts).forEach(([name, cost]) => { sum.parts[name] = (sum.parts[name] || 0) + cost; });
    return sum;
  }, { kwh: 0, cost: 0, peakCost: 0, parts: {} });
}

function renderCostCard() {
  const days = state.runtimeDays || [];
  if (!ratesSet()) {
    $("#costCard").replaceChildren(maintRow("Enter your rates in Settings → Energy", "", "muted"));
    $("#costHint").textContent = "";
    return;
  }
  const now = new Date();
  const todayDate = now.getFullYear() * 10000 + (now.getMonth() + 1) * 100 + now.getDate();
  const monthStart = Math.floor(todayDate / 100) * 100;
  const today = days.filter((day) => day.date === todayDate);
  const month = days.filter((day) => day.date > monthStart);
  const recent = sumEnergy(days);
  const share = recent.cost ? Math.round((recent.peakCost / recent.cost) * 100) : 0;
  const parts = recent.parts;
  $("#costCard").replaceChildren(
    maintRow("Today", costText(sumEnergy(today))),
    maintRow("This Month", costText(sumEnergy(month))),
    maintRow(`Last ${days.length} Days`, costText(recent)),
    maintRow("On-Peak Share", `${share}% of cost`, share > 25 ? "due" : ""),
    ...[["Heater", parts.heater], ["Jets", parts.jets], ["Standby", parts.standby]]
      .map(([label, cost]) => maintRow(`${label}, ${days.length} days`, moneyText(cost || 0), "muted")),
  );
  $("#costHint").textContent = "Estimated from run time and the rates and power figures in Settings → Energy; " +
    "changing them reprices past days too. Time before peak tracking began counts as off-peak.";
}

function renderCosts() {
  if (state.runtimeDays) {
    renderOverviewToday();
    renderRuntime();
  }
}

function fillEnergySettings(config) {
  const tou = config.tou || {};
  const rates = tou.rates || {};
  const power = config.power || {};
  $("#touPeakStart").value = String(tou.peakStart ?? 17);
  $("#touPeakEnd").value = String(tou.peakEnd ?? 21);
  $("#touWeekends").checked = tou.weekendsOffPeak !== false;
  $("#touHolidays").value = tou.holidays || "federal";
  $("#rateSummerStart").value = String(rates.summerStartMonth ?? 6);
  $("#rateSummerEnd").value = String(rates.summerEndMonth ?? 9);
  Object.entries(RateFields).forEach(([id, key]) => { $(`#${id}`).value = rates[key] ? rates[key] : ""; });
  Object.entries(PowerFields).forEach(([id, key]) => { $(`#${id}`).value = power[key] ?? ""; });
  $("#smartMinTemp").value = config.smart?.minTemp ?? "";
  $("#smartReadyTemp").value = config.smart?.readyTemp ?? "";
  $("#smartAuto").checked = config.smart?.auto !== false;
}

function setupEnergySettings() {
  const options = (select, values, label) => select.replaceChildren(...values.map((value) => {
    const option = document.createElement("option");
    option.value = String(value);
    option.textContent = label(value);
    return option;
  }));
  const hours = Array.from({ length: 24 }, (_, hour) => hour);
  options($("#touPeakStart"), hours, (hour) => clock12Label(hour));
  options($("#touPeakEnd"), hours.map((hour) => hour + 1), (hour) => (hour === 24 ? "12:00 AM (midnight)" : clock12Label(hour)));
  [$("#rateSummerStart"), $("#rateSummerEnd")].forEach((select) => options(select, MonthNames.map((_, i) => i + 1), (month) => MonthNames[month - 1]));

  $("#touPeakStart").addEventListener("change", (event) => saveConfig({ tou: { peakStart: Number(event.target.value) } }));
  $("#touPeakEnd").addEventListener("change", (event) => saveConfig({ tou: { peakEnd: Number(event.target.value) } }));
  $("#touWeekends").addEventListener("change", (event) => saveConfig({ tou: { weekendsOffPeak: event.target.checked } }));
  $("#touHolidays").addEventListener("change", (event) => saveConfig({ tou: { holidays: event.target.value } }));
  $("#rateSummerStart").addEventListener("change", (event) => saveConfig({ tou: { rates: { summerStartMonth: Number(event.target.value) } } }));
  $("#rateSummerEnd").addEventListener("change", (event) => saveConfig({ tou: { rates: { summerEndMonth: Number(event.target.value) } } }));
  const numberField = (id, save) => $(`#${id}`).addEventListener("change", (event) => {
    const text = event.target.value.trim();
    const value = text === "" ? 0 : Number(text);
    if (!Number.isFinite(value) || value < 0) {
      showToast("Enter a number of 0 or more");
      return;
    }
    save(value);
  });
  Object.entries(RateFields).forEach(([id, key]) => numberField(id, (value) => saveConfig({ tou: { rates: { [key]: value } } }, "Rate saved")));
  numberField("smartMinTemp", (value) => saveConfig({ smart: { minTemp: Math.round(value) } }));
  numberField("smartReadyTemp", (value) => saveConfig({ smart: { readyTemp: Math.round(value) } }, "Ready temp saved"));
  $("#smartAuto").addEventListener("change", (event) => saveConfig({ smart: { auto: event.target.checked } }));
  Object.entries(PowerFields).forEach(([id, key]) => numberField(id, (value) =>
    saveConfig({ power: { [key]: WholeNumberPower.includes(key) ? Math.round(value) : value } }, "Power saved")));
}

// ── settings auto-save ──

// Sends only the changed fields; the controller keeps everything else as it was.
async function saveConfig(changes, savedText = "Saved") {
  try {
    const message = await requestJson("/api/config", { method: "POST", body: JSON.stringify(changes) });
    state.config = message.data;
    showToast(savedText);
    renderCosts();
    return true;
  } catch (error) {
    showToast(error.message);
    return false;
  }
}

// Saves once both fields hold numbers, or clears the location when both are blank. A half-typed
// pair waits for the other field.
async function saveLocation() {
  const form = $("#settingsForm");
  const lat = field(form, "latitude").value.trim();
  const lon = field(form, "longitude").value.trim();
  if ((lat === "") !== (lon === "")) return;
  if (lat && (!Number.isFinite(Number(lat)) || !Number.isFinite(Number(lon)))) {
    showToast("Enter latitude and longitude as numbers");
    return;
  }
  const saved = await saveConfig({ location: lat ? { lat: Number(lat), lon: Number(lon) } : {} },
    lat ? "Location saved" : "Location cleared");
  if (saved) refreshWeatherForLocation();
}

async function savePassword() {
  const form = $("#settingsForm");
  const password = field(form, "authPassword").value;
  if (!password) {
    showToast("Type a password first");
    return;
  }
  if (await saveConfig({ authPassword: password, authEnabled: true }, "Password set")) {
    field(form, "authPassword").value = "";
  }
}

function setupAutoSaveSettings() {
  const form = $("#settingsForm");
  $("#timezonePreset").addEventListener("change", (event) => {
    if (event.target.value !== "custom") saveConfig({ timezone: event.target.value }, "Timezone saved");
  });
  field(form, "timezone").addEventListener("change", (event) => {
    const rule = event.target.value.trim();
    if (rule) saveConfig({ timezone: rule }, "Timezone saved");
  });
  field(form, "setTime").addEventListener("change", (event) => {
    saveConfig({ capabilities: { setTime: event.target.checked } });
  });
  field(form, "showJet2").addEventListener("change", (event) => {
    saveConfig({ capabilities: { hideJet2: !event.target.checked } });
  });
  ["latitude", "longitude"].forEach((name) => field(form, name).addEventListener("change", saveLocation));
  field(form, "authEnabled").addEventListener("change", (event) => {
    // Turning protection on needs a password; until one is set it only opens the password row.
    if (!event.target.checked) {
      saveConfig({ authEnabled: false }, "Password turned off");
    } else if (state.config?.authPasswordSet) {
      saveConfig({ authEnabled: true }, "Password turned on");
    } else {
      showToast("Set a password to turn this on");
      field(form, "authPassword").focus();
    }
  });
  $("#setPassword").addEventListener("click", savePassword);
  field(form, "authPassword").addEventListener("keydown", (event) => {
    if (event.key === "Enter") savePassword();
  });
}

function setupLocationSettings() {
  const available = "geolocation" in navigator && window.isSecureContext;
  $("#useLocationRow").hidden = !available;
  $("#useLocation").addEventListener("click", () => {
    navigator.geolocation.getCurrentPosition(
      (position) => {
        const form = $("#settingsForm");
        field(form, "latitude").value = position.coords.latitude.toFixed(2);
        field(form, "longitude").value = position.coords.longitude.toFixed(2);
        saveLocation();
      },
      () => showToast("Couldn't get this device's location"),
      { timeout: 10000 },
    );
  });
}

function setupFilterSettings() {
  $("#editFilter1").addEventListener("click", () => openFilterSheet(1));
  $("#editFilter2").addEventListener("click", () => openFilterSheet(2));
  ["fsStartHour", "fsStartMinute", "fsStartPeriod", "fsDurHour", "fsDurMinute"].forEach((id) => {
    $(`#${id}`).addEventListener("change", updateFilterSheetEnd);
  });
  $("#filterSheetCancel").addEventListener("click", closeFilterSheet);
  $("#filterSheetSave").addEventListener("click", saveFilterSheet);
  $("#filterSheet").addEventListener("click", (event) => {
    if (event.target.id === "filterSheet") closeFilterSheet();
  });
  $("#filter2Toggle").addEventListener("change", (event) => {
    const filters = state.data?.filterCycles || {};
    if (event.target.checked && !filters.cycle2Duration && !filters.cycle2DurationMinute) {
      event.target.checked = false;
      openFilterSheet(2);  // needs a time and run length before it can turn on
      return;
    }
    sendCommand({
      action: "setFilter2",
      enabled: event.target.checked,
      startHour: filters.cycle2Start ?? 0,
      startMinute: filters.cycle2StartMinute ?? 0,
      durationHour: filters.cycle2Duration ?? 0,
      durationMinute: filters.cycle2DurationMinute ?? 0,
    });
  });
}

function setupEvents() {
  $$(".nav-btn").forEach((button) => {
    button.addEventListener("click", () => showView(button.dataset.view));
  });

  $("#tempOrb").addEventListener("click", openTempModal);
  $("#modalDown").addEventListener("click", () => adjustModalTemp(-1));
  $("#modalUp").addEventListener("click", () => adjustModalTemp(1));
  $("#modalCancel").addEventListener("click", closeTempModal);
  $("#tempModal").addEventListener("click", (event) => {
    if (event.target === event.currentTarget) {
      closeTempModal();
    }
  });
  $("#modalSet").addEventListener("click", () => {
    if (!Number.isFinite(state.modalTemp)) return;
    const value = state.modalTemp;
    closeTempModal();
    sendCommand({ action: "setTemp", value });
  });

  $$("[data-action]").forEach((button) => {
    button.addEventListener("click", () => {
      const action = button.dataset.action;
      console.log("[bp100g2] control clicked", action, state.data);
      if (action === "light") {
        sendCommand({ action, value: !state.data?.light });
      } else if (action === "hold") {
        sendCommand({ action, value: !state.data?.hold });
      } else {
        sendCommand({ action });
      }
    });
  });

  $("#syncTime").addEventListener("click", () => sendCommand({ action: "syncTime" }));

  setupJetButtons();
  setupMaintenance();
  setupWater();
  setupSoak();

  $$(".subtab").forEach((tab) => tab.addEventListener("click", () => showSubtab(tab.dataset.subtab)));
  $$(".view").forEach((view) => {
    try {
      const saved = sessionStorage.getItem(`subtab:${view.id}`);
      if (saved && view.querySelector(`.subtab[data-subtab="${saved}"]`)) showSubtab(saved);
    } catch (error) {
      // No stored sub-tab; the first one stays selected.
    }
  });
  try {
    const savedView = sessionStorage.getItem("view");
    if (savedView) showView(savedView);
  } catch (error) {
    // No stored page; Home stays selected.
  }
  field($("#settingsForm"), "authEnabled").addEventListener("change", syncDependentFields);
  $("#refreshSpaInfo").addEventListener("click", () => sendCommand({ action: "refreshSpaInfo" }));
  $("#timezonePreset").addEventListener("change", (event) => {
    const custom = event.target.value === "custom";
    $("#timezoneCustomRow").hidden = !custom;
    if (custom) {
      field($("#settingsForm"), "timezone").focus();
    } else {
      field($("#settingsForm"), "timezone").value = event.target.value;
    }
  });
  $("#holdResume").addEventListener("click", () => sendCommand({ action: "hold", value: false }));
  $("#reminderDismiss").addEventListener("click", () => sendCommand({ action: "clearReminder" }));
  $("#refreshFaults").addEventListener("click", readFaultLog);
  $("#commandClose").addEventListener("click", hideCommandModal);

  $("#startCapture").addEventListener("click", async () => {
    const result = await sendCommand({ action: "startStatusCapture" });
    if (result) {
      startCapturePolling();
      await refreshStatusCapture();
    }
  });

  $("#stopCapture").addEventListener("click", async () => {
    const result = await sendCommand({ action: "stopStatusCapture" });
    if (result) {
      stopCapturePolling();
    }
  });

  $("#copyCapture").addEventListener("click", async () => {
    const text = $("#captureLog").value;
    if (!text) {
      showToast("Capture is empty");
      return;
    }
    try {
      await navigator.clipboard.writeText(text);
      showToast("Capture copied");
    } catch (error) {
      $("#captureLog").select();
      showToast("Select and copy capture");
    }
  });

  $("#clearCapture").addEventListener("click", async () => {
    await requestJson("/api/status-capture", { method: "DELETE" });
    $("#captureLog").value = "";
    $("#captureCount").textContent = "0 lines";
    showToast("Capture cleared");
    if (state.data?.statusCaptureActive) {
      startCapturePolling();
    }
  });

  setupFilterSettings();
  setupLocationSettings();
  setupTempRange();
  let resizeTimer = null;
  window.addEventListener("resize", () => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(renderRuntime, 150);
  });

  setupAutoSaveSettings();
  setupEnergySettings();
  // Every setting saves on its own; Enter in a field must not submit the form.
  $("#settingsForm").addEventListener("submit", (event) => event.preventDefault());
}

setupEvents();
connectWs();
loadConfig().catch((error) => showToast(error.message));

if ("serviceWorker" in navigator) {
  window.addEventListener("load", () => {
    navigator.serviceWorker.register("/sw.js").catch((error) => {
      console.warn("[bp100g2] service worker registration failed", error);
    });
  });
}
