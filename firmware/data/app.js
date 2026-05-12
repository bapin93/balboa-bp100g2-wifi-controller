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

function timeLabel(hour, minute = 0) {
  if (!Number.isFinite(hour)) return "--";
  const normalized = ((Math.round(hour) % 24) + 24) % 24;
  const normalizedMinute = Number.isFinite(minute) ? Math.max(0, Math.min(59, Math.round(minute))) : 0;
  return `${String(normalized).padStart(2, "0")}:${String(normalizedMinute).padStart(2, "0")}`;
}

function durationLabel(hour, minute = 0) {
  if (!Number.isFinite(hour)) return "--";
  const normalizedHour = Math.max(0, Math.round(hour));
  const normalizedMinute = Number.isFinite(minute) ? Math.max(0, Math.min(59, Math.round(minute))) : 0;
  if (normalizedMinute === 0) {
    return `${normalizedHour}h`;
  }
  return `${normalizedHour}h ${normalizedMinute}m`;
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

function filterLabel(start, startMinute, duration, durationMinute = 0, enabled = true) {
  const hasDuration = Number.isFinite(duration) && (duration > 0 || (Number.isFinite(durationMinute) && durationMinute > 0));
  if (!enabled || !Number.isFinite(start) || !hasDuration) {
    return "Off";
  }
  return `${timeLabel(start, startMinute)} / ${durationLabel(duration, durationMinute)}`;
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
  $("#heating").textContent = data.heating ? "On" : data.heatPending ? "Waiting" : "Off";
  $("#jet1State").textContent = jetLabel(data.jet1, data.jet1Speed);
  $("#jet2State").textContent = jetLabel(data.jet2, data.jet2Speed);
  $("#jet1ButtonState").textContent = jetLabel(data.jet1, data.jet1Speed);
  $("#jet2ButtonState").textContent = jetLabel(data.jet2, data.jet2Speed);
  $("#lightState").textContent = data.light ? "On" : "Off";
  $("#heatMode").textContent = ["Ready", "Rest", "Ready-in-Rest"][data.heatMode] || "Unknown";
  $("#spaTime").textContent = data.spaTime || "--";
  $("#lastFrame").textContent = data.valid ? "Live" : "Waiting";
  $("#captureState").textContent = data.statusCaptureActive ? "Capturing" : "Stopped";
  $("#captureCount").textContent = `${data.statusCaptureLines ?? 0} lines`;
  $("#startCapture").disabled = !!data.statusCaptureActive;
  $("#stopCapture").disabled = !data.statusCaptureActive;
  const filters = data.filterCycles || {};
  $("#filterCycle1").textContent = filterLabel(
    filters.cycle1Start,
    filters.cycle1StartMinute,
    filters.cycle1Duration,
    filters.cycle1DurationMinute,
  );
  $("#filterCycle2").textContent = filterLabel(
    filters.cycle2Start,
    filters.cycle2StartMinute,
    filters.cycle2Duration,
    filters.cycle2DurationMinute,
    filters.cycle2Enabled,
  );
  if (state.filterCycleRefreshPending && filters.valid === true) {
    applyFilterFields(filters);
    state.filterCycleRefreshPending = false;
  }

  const caps = data.capabilities || {};
  $("#syncTime").hidden = caps.setTime === false;
  $$("[data-action]").forEach((button) => {
    const action = button.dataset.action;
    const supported = action === "heatMode" ? caps.heatMode : caps[action] !== false;
    button.hidden = !supported;
    const active = action === "jet1" ? data.jet1 : action === "jet2" ? data.jet2 : action === "light" ? data.light : false;
    button.classList.toggle("active", active);
    button.setAttribute("aria-pressed", active ? "true" : "false");
  });

  if (data.statusCaptureActive && !state.capturePollTimer) {
    startCapturePolling();
  } else if (!data.statusCaptureActive && state.capturePollTimer) {
    stopCapturePolling();
  }

  updateCommandWait(data);
}

function clampTemp(value) {
  return Math.max(50, Math.min(104, Math.round(value)));
}

function currentTargetTemp() {
  const value = state.data?.targetSetTemp ?? state.data?.setTemp;
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
  state.commandWait = null;
  state.commandResolve = null;
  $("#commandModal").classList.add("hidden");
  $("#commandModal").setAttribute("aria-hidden", "true");
}

function completeCommandModal() {
  setCommandModal("success", "Success!");
  const resolve = state.commandResolve;
  state.commandWait = null;
  state.commandResolve = null;
  if (resolve) {
    resolve();
  }
  state.commandSuccessTimer = setTimeout(hideCommandModal, 1600);
}

function failCommandModal(message) {
  setCommandModal("error", "Command failed", message);
  state.commandWait = null;
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
    case "heatMode":
      return { title: "Setting Heat Mode", details: "Cycling mode" };
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
  if (command.action === "setTemp") {
    return null;
  }
  if (["setFilter1", "setFilter2"].includes(command.action)) {
    return "filter";
  }
  return null;
}

function usesBlockingCommandModal(command) {
  return ["setTemp", "setFilter1", "setFilter2"].includes(command.action);
}

function waitForCommandCompletion(type) {
  state.commandWait = { type, seenActive: type === "filter" };
  return new Promise((resolve) => {
    state.commandResolve = resolve;
  });
}

function updateCommandWait(data) {
  if (!state.commandWait) return;
  if (state.commandWait.type === "filter") {
    if (data.filterProgramActive === true) {
      state.commandWait.seenActive = true;
    } else if (state.commandWait.seenActive && data.filterProgramActive === false) {
      if (data.filterProgramFailed) {
        failCommandModal(`${data.filterProgramError || "Filter command failed"}\nSaved a command log on the controller.`);
      } else {
        completeCommandModal();
      }
    }
  } else if (state.commandWait.type === "targetTemp") {
    const active = data.targetSetTemp !== null && data.targetSetTemp !== undefined;
    if (active) {
      state.commandWait.seenActive = true;
    } else if (state.commandWait.seenActive && !active) {
      completeCommandModal();
    }
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
      await waitForCommandCompletion(waitType);
    } else if (useModal) {
      completeCommandModal();
      await new Promise((resolve) => setTimeout(resolve, 1600));
    } else {
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

function renderCommandLogs(logs) {
  const list = $("#commandLogList");
  list.replaceChildren();
  $("#commandLogState").textContent = `${logs.length} logs`;
  if (!logs.length) {
    const item = document.createElement("li");
    item.className = "command-log-empty";
    item.textContent = "No logs saved";
    list.append(item);
    return;
  }
  logs.forEach((log) => {
    const item = document.createElement("li");
    const link = document.createElement("a");
    link.href = `/api/command-logs?file=${encodeURIComponent(log.name)}`;
    link.target = "_blank";
    link.rel = "noopener";
    link.textContent = log.name;
    item.append(link);
    list.append(item);
  });
}

async function loadCommandLogs() {
  const message = await requestJson("/api/command-logs");
  const logs = Array.isArray(message.logs) ? message.logs : [];
  logs.sort((a, b) => String(b.name || "").localeCompare(String(a.name || "")));
  renderCommandLogs(logs.filter((log) => log.name));
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
  field(form, "backlightTimeout").value = state.config.backlightTimeout ?? 0;
  field(form, "authEnabled").checked = !!state.config.authEnabled;
  const caps = state.config.capabilities || {};
  ["jet1", "jet2", "light", "heatMode", "setTime", "filterCycles", "backlight"].forEach((key) => {
    field(form, key).checked = !!caps[key];
  });
  applyFilterFields(state.config.filterCycles || {});
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
    button.addEventListener("click", () => {
      $$(".nav-btn").forEach((tab) => tab.classList.remove("active"));
      $$(".view").forEach((view) => view.classList.remove("active"));
      button.classList.add("active");
      $(`#${button.dataset.view}`).classList.add("active");
      if (button.dataset.view === "diagnostics") {
        loadCommandLogs().catch((error) => showToast(error.message));
      }
    });
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
      if (action === "heatMode") {
        const next = ((state.data?.heatMode || 0) + 1) % 3;
        sendCommand({ action, value: next });
      } else {
        sendCommand({ action, value: true });
      }
    });
  });

  $("#syncTime").addEventListener("click", () => sendCommand({ action: "syncTime" }));
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

  $("#refreshCommandLogs").addEventListener("click", () => {
    loadCommandLogs().catch((error) => showToast(error.message));
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
    const filter1 = filter1Values(form);
    const filter2 = filter2Values(form);
    const submit = form.querySelector("button[type='submit']");
    showToast("Saving settings...");
    submit.disabled = true;
    const body = {
      timezone: field(form, "timezone").value.trim(),
      backlightTimeout: Number(field(form, "backlightTimeout").value || 0),
      authEnabled: field(form, "authEnabled").checked,
      capabilities: {
        jet1: field(form, "jet1").checked,
        jet2: field(form, "jet2").checked,
        light: field(form, "light").checked,
        heatMode: field(form, "heatMode").checked,
        setTime: field(form, "setTime").checked,
        filterCycles: field(form, "filterCycles").checked,
        backlight: field(form, "backlight").checked,
      },
      filterCycles: {
        cycle1Start: filter1.startHour,
        cycle1StartMinute: filter1.startMinute,
        cycle1Duration: filter1.durationHour,
        cycle1DurationMinute: filter1.durationMinute,
        cycle2Enabled: filter2.enabled,
        cycle2Start: filter2.startHour,
        cycle2StartMinute: filter2.startMinute,
        cycle2Duration: filter2.durationHour,
        cycle2DurationMinute: filter2.durationMinute,
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
      await refreshFilterCyclesFromSpa();
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
loadCommandLogs().catch((error) => showToast(error.message));

if ("serviceWorker" in navigator) {
  window.addEventListener("load", () => {
    navigator.serviceWorker.register("/sw.js").catch((error) => {
      console.warn("[bp100g2] service worker registration failed", error);
    });
  });
}
