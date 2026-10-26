const state = {
  data: null,
  config: null,
  ws: null,
  modalTemp: null,
  toastTimer: null,
  filterCycleRefreshStarted: false,
  filterCycleRefreshPending: false,
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
  runtimeLoadedAt: 0,
  faultLog: null,
  maintenance: null,
  maintClockOffset: 0,
  maintLoadedAt: 0,
  maintSheet: null,
  maintHistoryExpanded: false,
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
  setValue("ovWater", tempLabel(data.waterTemp));
  setValue("ovSet", tempLabel(data.setTemp));
  const toGo = Math.round((data.setTemp ?? 0) - (data.waterTemp ?? 0));
  const heroStatus = $("#ovHeatStatus");
  heroStatus.textContent = !data.valid ? "Waiting for the spa"
    : data.hold ? "Paused for hold"
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
    const days = latest.daysAgo === 0 ? "today" : latest.daysAgo === 1 ? "yesterday" : `${latest.daysAgo} days ago`;
    setValue("ovLastFault", `${FaultText[latest.code] || `Code ${latest.code}`} · ${days}`);
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
}

function filter1Values(form) {
  return {
    startHour: toHour24(
      Number(field(form, "filterCycle1StartHour12").value || 12),
      field(form, "filterCycle1StartPeriod").value,
    ),
    startMinute: Number(field(form, "filterCycle1StartMinute").value || 0),
    durationHour: Number(field(form, "filterCycle1Duration").value || 0),
    durationMinute: Number(field(form, "filterCycle1DurationMinute").value || 0),
  };
}

function filter2Values(form) {
  return {
    enabled: field(form, "filterCycle2Enabled").checked,
    startHour: toHour24(
      Number(field(form, "filterCycle2StartHour12").value || 12),
      field(form, "filterCycle2StartPeriod").value,
    ),
    startMinute: Number(field(form, "filterCycle2StartMinute").value || 0),
    durationHour: Number(field(form, "filterCycle2Duration").value || 0),
    durationMinute: Number(field(form, "filterCycle2DurationMinute").value || 0),
  };
}

function updateFilterEndTime(values, selector) {
  const durationMinutes = values.durationHour * 60 + values.durationMinute;
  if (durationMinutes <= 0) {
    $(selector).textContent = "--";
    return;
  }
  const endTotalMinutes = (values.startHour * 60 + values.startMinute + durationMinutes) % (24 * 60);
  $(selector).textContent = clock12Label(Math.floor(endTotalMinutes / 60), endTotalMinutes % 60);
}

function updateFilter1EndTime() {
  updateFilterEndTime(filter1Values($("#settingsForm")), "#f1EndTime");
}

function updateFilter2EndTime() {
  const values = filter2Values($("#settingsForm"));
  updateFilterEndTime(values, "#f2EndTime");
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
  $("#waterTemp").textContent = tempLabel(data.waterTemp);
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
  $("#captureState").textContent = data.statusCaptureActive ? "Capturing" : "Stopped";
  $("#captureSummary").textContent = data.statusCaptureActive ? "Capturing" : "For troubleshooting";
  $("#captureCount").textContent = `${data.statusCaptureLines ?? 0} lines`;
  $("#startCapture").disabled = !!data.statusCaptureActive;
  $("#stopCapture").disabled = !data.statusCaptureActive;
  const filters = data.filterCycles || {};
  if (state.filterCycleRefreshPending && filters.valid === true) {
    applyFilterFields(filters);
    state.filterCycleRefreshPending = false;
  }

  const caps = data.capabilities || {};
  $("#syncTime").hidden = caps.setTime === false;
  $$("[data-action]").forEach((button) => {
    const action = button.dataset.action;
    button.hidden = caps[action] === false;
    const active = action === "light" ? data.light : action === "hold" ? data.hold : false;
    button.classList.toggle("active", !!active);
    button.setAttribute("aria-pressed", active ? "true" : "false");
  });
  $("#holdButtonState").textContent = data.hold ? `${data.holdMinutes} min` : "";
  renderPumps(data, caps);
  renderOverview(data, caps);
  const maintRefreshMs = $("#maintenance").classList.contains("active") ? 60000 : 600000;
  if (Date.now() - state.maintLoadedAt > maintRefreshMs) {
    state.maintLoadedAt = Date.now();  // throttle; pumped hours only change slowly
    loadMaintenance().catch((error) => console.warn("[bp100g2] maintenance failed", error));
  }
  renderBanners(data);
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

function hoursMinutes(seconds) {
  const totalMinutes = Math.round((seconds || 0) / 60);
  const hours = Math.floor(totalMinutes / 60);
  const minutes = totalMinutes % 60;
  return hours ? `${hours}h ${String(minutes).padStart(2, "0")}m` : `${minutes}m`;
}

function dayLabel(date) {
  const year = Math.floor(date / 10000);
  const month = Math.floor(date / 100) % 100;
  const day = date % 100;
  return new Date(year, month - 1, day).toLocaleDateString(undefined, { weekday: "short", month: "short", day: "numeric" });
}

async function loadRuntime() {
  const message = await requestJson("/api/runtime");
  state.runtimeDays = message.days || [];
  state.runtimeLoadedAt = Date.now();
  if (state.data) renderOverview(state.data, state.data.capabilities || {});
  const days = state.runtimeDays.slice().reverse().slice(0, 14);
  const body = $("#runtimeTable tbody");
  body.replaceChildren();
  if (!days.length) {
    const row = body.insertRow();
    const cell = row.insertCell();
    cell.colSpan = 5;
    cell.className = "empty";
    cell.textContent = "No runtime recorded yet";
    return;
  }
  days.forEach((day) => {
    const row = body.insertRow();
    [dayLabel(day.date), hoursMinutes(day.heatingSec), hoursMinutes(day.pumpLowSec),
     hoursMinutes(day.pumpHighSec), hoursMinutes(day.filterSec)].forEach((text) => {
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

function faultWhen(entry) {
  const days = entry.daysAgo === 0 ? "Today" : entry.daysAgo === 1 ? "Yesterday" : `${entry.daysAgo} days ago`;
  return `${days} at ${clock12Label(entry.hour, entry.minute)}`;
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

function connectWs() {
  const protocol = location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${protocol}://${location.host}/ws`);
  state.ws = ws;

  ws.addEventListener("open", () => setConnection("Live", true));
  ws.addEventListener("close", () => {
    setConnection("Reconnecting");
    setTimeout(connectWs, 1500);
  });
  ws.addEventListener("message", (event) => {
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
  state.config = message.data;
  const form = $("#settingsForm");
  field(form, "timezone").value = state.config.timezone || "";
  syncTimezonePreset();
  field(form, "authEnabled").checked = !!state.config.authEnabled;
  const caps = state.config.capabilities || {};
  syncDependentFields();
  ["setTime"].forEach((key) => {
    field(form, key).checked = !!caps[key];
  });
  applyFilterFields(state.config.filterCycles || {});
}

function syncDependentFields() {
  const form = $("#settingsForm");
  $("#filter2Fields").hidden = !field(form, "filterCycle2Enabled").checked;
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
  const needsAttention = tasks.some((task) => taskStatus(task).rank === 0) ||
    filters.some((filter) => filter.inTub && filterCleanDue(filter));
  $('.nav-btn[data-view="maintenance"]').classList.toggle("has-alert", needsAttention);
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

  // Newest first by the date it happened, which can differ from the order it was recorded in.
  const events = (data.events || [])
    .map((event, index) => ({ ...event, index }))
    .sort((a, b) => b.ts - a.ts || b.index - a.index);
  const shown = state.maintHistoryExpanded ? events : events.slice(0, MaintHistoryPreview);
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
    item.textContent = (EventText[event.type] || EventText.added)(event.name);
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
  $("#maintHistory").replaceChildren(...items);
  const toggle = $("#maintHistoryToggle");
  toggle.hidden = events.length <= MaintHistoryPreview;
  toggle.textContent = state.maintHistoryExpanded ? "Show less" : `Show all (${events.length})`;
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
  $("#maintHistoryToggle").addEventListener("click", () => {
    state.maintHistoryExpanded = !state.maintHistoryExpanded;
    renderMaintenance();
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

function applyFilterFields(filters) {
  const form = $("#settingsForm");
  const filter1Start = toClock12(filters.cycle1Start ?? 8);
  field(form, "filterCycle1StartHour12").value = filter1Start.hour;
  field(form, "filterCycle1StartPeriod").value = filter1Start.period;
  field(form, "filterCycle1StartMinute").value = filters.cycle1StartMinute ?? 0;
  field(form, "filterCycle1Duration").value = filters.cycle1Duration ?? 2;
  field(form, "filterCycle1DurationMinute").value = filters.cycle1DurationMinute ?? 0;
  updateFilter1EndTime();
  field(form, "filterCycle2Enabled").checked = !!filters.cycle2Enabled;
  const filter2Start = toClock12(filters.cycle2Start ?? 20);
  field(form, "filterCycle2StartHour12").value = filter2Start.hour;
  field(form, "filterCycle2StartPeriod").value = filter2Start.period;
  field(form, "filterCycle2StartMinute").value = filters.cycle2StartMinute ?? 0;
  field(form, "filterCycle2Duration").value = filters.cycle2Duration ?? 0;
  field(form, "filterCycle2DurationMinute").value = filters.cycle2DurationMinute ?? 0;
  updateFilter2EndTime();
  syncDependentFields();
}

async function refreshFilterCyclesFromSpa() {
  if (state.filterCycleRefreshStarted) {
    return;
  }
  state.filterCycleRefreshStarted = true;
  state.filterCycleRefreshPending = true;
  try {
    await requestJson("/api/cmd", {
      method: "POST",
      body: JSON.stringify({ action: "readFilterCycles" }),
    });
    [700, 1600, 3000].forEach((delay) => {
      setTimeout(() => loadConfig().catch((error) => showToast(error.message)), delay);
    });
  } catch (error) {
    state.filterCycleRefreshPending = false;
    console.warn("[bp100g2] filter cycle refresh failed", error);
  }
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
  ["filterCycle2Enabled", "authEnabled"].forEach((name) => {
    field($("#settingsForm"), name).addEventListener("change", syncDependentFields);
  });
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

  [
    "filterCycle1StartHour12",
    "filterCycle1StartMinute",
    "filterCycle1StartPeriod",
    "filterCycle1Duration",
    "filterCycle1DurationMinute",
  ].forEach((name) => {
    field($("#settingsForm"), name).addEventListener("input", updateFilter1EndTime);
    field($("#settingsForm"), name).addEventListener("change", updateFilter1EndTime);
  });

  [
    "filterCycle2StartHour12",
    "filterCycle2StartMinute",
    "filterCycle2StartPeriod",
    "filterCycle2Duration",
    "filterCycle2DurationMinute",
  ].forEach((name) => {
    field($("#settingsForm"), name).addEventListener("input", updateFilter2EndTime);
    field($("#settingsForm"), name).addEventListener("change", updateFilter2EndTime);
  });

  $("#applyFilter1").addEventListener("click", () => {
    const form = $("#settingsForm");
    const values = filter1Values(form);
    const command = {
      action: "setFilter1",
      startHour: values.startHour,
      startMinute: values.startMinute,
      durationHour: values.durationHour,
      durationMinute: values.durationMinute,
    };
    console.log("[bp100g2] apply filter 1", command, state.data);
    sendCommand(command).then((result) => {
      if (result) loadConfig().catch((error) => showToast(error.message));
    });
  });

  $("#applyFilter2").addEventListener("click", () => {
    const form = $("#settingsForm");
    const values = filter2Values(form);
    const command = {
      action: "setFilter2",
      enabled: values.enabled,
      startHour: values.startHour,
      startMinute: values.startMinute,
      durationHour: values.durationHour,
      durationMinute: values.durationMinute,
    };
    console.log("[bp100g2] apply filter 2", command, state.data);
    sendCommand(command).then((result) => {
      if (result) loadConfig().catch((error) => showToast(error.message));
    });
  });

  $("#settingsForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    const submit = form.querySelector("button[type='submit']");
    showToast("Saving settings...");
    submit.disabled = true;
    const body = {
      timezone: field(form, "timezone").value.trim(),
      authEnabled: field(form, "authEnabled").checked,
      capabilities: {
        setTime: field(form, "setTime").checked,
      },
    };
    if (field(form, "authPassword").value) {
      body.authPassword = field(form, "authPassword").value;
    }
    try {
      await requestJson("/api/config", { method: "POST", body: JSON.stringify(body) });
      field(form, "authPassword").value = "";
      showToast("Settings saved");
      await loadConfig();
    } catch (error) {
      showToast(error.message);
    } finally {
      submit.disabled = false;
    }
  });
}

setupEvents();
connectWs();
loadConfig()
  .then(refreshFilterCyclesFromSpa)
  .catch((error) => showToast(error.message));

if ("serviceWorker" in navigator) {
  window.addEventListener("load", () => {
    navigator.serviceWorker.register("/sw.js").catch((error) => {
      console.warn("[bp100g2] service worker registration failed", error);
    });
  });
}
