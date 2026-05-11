const state = {
  data: null,
  config: null,
  ws: null,
};

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => Array.from(document.querySelectorAll(selector));

function tempLabel(value) {
  return Number.isFinite(value) ? `${Math.round(value)}°F` : "--";
}

function hourLabel(hour) {
  if (!Number.isFinite(hour)) return "--";
  const normalized = ((Math.round(hour) % 24) + 24) % 24;
  return `${String(normalized).padStart(2, "0")}:00`;
}

function filterLabel(start, duration, enabled = true) {
  if (!enabled || !Number.isFinite(start) || !Number.isFinite(duration) || duration <= 0) {
    return "Off";
  }
  return `${hourLabel(start)} / ${Math.round(duration)}h`;
}

function setConnection(text, ok = false) {
  const el = $("#connection");
  el.textContent = text;
  el.classList.toggle("ok", ok);
}

function showToast(text) {
  $("#toast").textContent = text;
}

function field(form, name) {
  return form.elements.namedItem(name);
}

function renderState(data) {
  state.data = data;
  $("#waterTemp").textContent = tempLabel(data.waterTemp);
  $("#setTemp").textContent = tempLabel(data.setTemp);
  $("#heating").textContent = data.heating ? "On" : "Off";
  $("#heatMode").textContent = ["Ready", "Rest", "Ready-in-Rest"][data.heatMode] || "Unknown";
  $("#spaTime").textContent = data.spaTime || "--";
  $("#lastFrame").textContent = data.valid ? "Live" : "Waiting";
  const filters = data.filterCycles || {};
  $("#filterCycle1").textContent = filterLabel(filters.cycle1Start, filters.cycle1Duration);
  $("#filterCycle2").textContent = filterLabel(
    filters.cycle2Start,
    filters.cycle2Duration,
    filters.cycle2Enabled,
  );

  const caps = data.capabilities || {};
  $$("[data-action]").forEach((button) => {
    const action = button.dataset.action;
    const supported = action === "heatMode" ? caps.heatMode : caps[action] !== false;
    button.hidden = !supported;
  });
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

async function sendCommand(command) {
  try {
    const result = await requestJson("/api/cmd", {
      method: "POST",
      body: JSON.stringify(command),
    });
    showToast(result.message || result.status);
  } catch (error) {
    showToast(error.message);
  }
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
  ["jet1", "jet2", "light", "heatMode", "setTime", "filterCycles"].forEach((key) => {
    field(form, key).checked = !!caps[key];
  });
  const filters = state.config.filterCycles || {};
  field(form, "filterCycle1Start").value = filters.cycle1Start ?? 8;
  field(form, "filterCycle1Duration").value = filters.cycle1Duration ?? 2;
  field(form, "filterCycle2Enabled").checked = !!filters.cycle2Enabled;
  field(form, "filterCycle2Start").value = filters.cycle2Start ?? 20;
  field(form, "filterCycle2Duration").value = filters.cycle2Duration ?? 0;
}

function setupEvents() {
  $$(".tab").forEach((button) => {
    button.addEventListener("click", () => {
      $$(".tab").forEach((tab) => tab.classList.remove("active"));
      $$(".view").forEach((view) => view.classList.remove("active"));
      button.classList.add("active");
      $(`#${button.dataset.view}`).classList.add("active");
    });
  });

  $("#tempDown").addEventListener("click", () => {
    if (!state.data || !Number.isFinite(state.data.setTemp)) return;
    sendCommand({ action: "setTemp", value: Math.round(state.data.setTemp) - 1 });
  });
  $("#tempUp").addEventListener("click", () => {
    if (!state.data || !Number.isFinite(state.data.setTemp)) return;
    sendCommand({ action: "setTemp", value: Math.round(state.data.setTemp) + 1 });
  });

  $$("[data-action]").forEach((button) => {
    button.addEventListener("click", () => {
      const action = button.dataset.action;
      if (action === "heatMode") {
        const next = ((state.data?.heatMode || 0) + 1) % 3;
        sendCommand({ action, value: next });
      } else {
        sendCommand({ action, value: true });
      }
    });
  });

  $("#syncTime").addEventListener("click", () => sendCommand({ action: "syncTime" }));

  $("#settingsForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    const form = event.currentTarget;
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
      },
      filterCycles: {
        cycle1Start: Number(field(form, "filterCycle1Start").value || 0),
        cycle1Duration: Number(field(form, "filterCycle1Duration").value || 0),
        cycle2Enabled: field(form, "filterCycle2Enabled").checked,
        cycle2Start: Number(field(form, "filterCycle2Start").value || 0),
        cycle2Duration: Number(field(form, "filterCycle2Duration").value || 0),
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
loadConfig().catch((error) => showToast(error.message));
