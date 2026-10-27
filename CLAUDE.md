# Balboa BP100G2 Custom WiFi Controller

## What This Is

An ESP32-based replacement for the official Balboa WiFi module. It plugs into the 4-pin Molex connector on the spa pack, speaks the Balboa RS-485 protocol, and serves a custom web app from its own flash. No cloud. No Home Assistant required (but MQTT for HA is an optional add-on).

---

## Hardware

### Components

- ESP32 (4MB flash, standard WROOM-32 or equivalent)
- MAX485 RS-485 transceiver (half-duplex)
- 12V→5V step-down buck converter (accepts 7–28V input; 15V from spa is fine)
- 4-pin female Molex connector

### Spa Connector Pinout (top-left → bottom-right)

```
PIN 1: GND
PIN 2: DATA-A  (RS-485 A / non-inverting)
PIN 3: DATA-B  (RS-485 B / inverting)
PIN 4: 15V
```

### Wiring

```
Spa PIN 1 (GND)  ──────────────────── GND rail
Spa PIN 2 (A)    ──── MAX485 A
Spa PIN 3 (B)    ──── MAX485 B
Spa PIN 4 (15V)  ──── Step-down IN+
                       Step-down OUT+ ──── ESP32 VIN (5V)
                       Step-down OUT- ──── GND rail

MAX485:
  RO  ──── ESP32 GPIO 16  (UART2 RX)
  DI  ──── ESP32 GPIO 17  (UART2 TX)
  RE  ──┐
  DE  ──┴── ESP32 GPIO 4  (HIGH = transmit, LOW = receive)
  VCC ──── ESP32 3.3V
  GND ──── GND rail
```

---

## Architecture Decisions (all finalized)

| Decision | Choice | Reason |
|---|---|---|
| Build system | PlatformIO, Arduino framework | Library ecosystem, OTA support |
| Protocol impl | Custom C++ (not ESPHome) | Full control, no HA dependency |
| Web app host | ESP32 LittleFS | No external server needed |
| Web app stack | Vanilla HTML/CSS/JS | No build step, small footprint |
| WiFi setup | WiFiManager captive portal | No hardcoded credentials |
| Ambient temp | Open-Meteo hourly forecast (free, no key) | Lat/lon only; feeds the heating model and planner |
| Time | NTP (built-in Arduino ESP32) | Needed for scheduling + spa sync |
| HA integration | Optional MQTT via PubSubClient | Off by default, configurable |

---

## Project Structure

```
firmware/
├── platformio.ini
├── src/
│   ├── main.cpp
│   ├── config.h                        # GPIO pins, baud rate, constants
│   ├── balboa/
│   │   ├── protocol.h/.cpp             # RS-485 frame parser & builder
│   │   └── spa.h/.cpp                  # SpaState struct + command queue
│   ├── network/
│   │   ├── web_server.h/.cpp           # ESPAsyncWebServer + WebSocket hub + REST
│   │   └── weather_client.h/.cpp       # Open-Meteo hourly forecast (own FreeRTOS task, plain HTTP)
│   ├── scheduler/                      # Smart heating; everything but smart_heat is plain C++ with native tests
│   │   ├── tou.h/.cpp                  # Peak windows, weekends, holiday sets
│   │   ├── thermal_model.h/.cpp        # Learns heater rate + heat-loss coefficient from measured runs
│   │   ├── heat_planner.h/.cpp         # DP planner: cheapest heater schedule meeting per-step min temps
│   │   ├── usage_model.h/.cpp          # Learned soak likelihood per half hour (workday / weekend)
│   │   ├── use_detector.h/.cpp         # "Someone is in the tub" from jets, light, blower
│   │   ├── heat_controller.h/.cpp      # Auto / Soak / Manual modes -> set temp to send
│   │   └── smart_heat.h/.cpp           # Glue: runs the controller against the spa, persistence, commands
│   └── storage/
│       ├── store.h/.cpp                # LittleFS/NVS read/write for everything below
│       ├── config_model.h/.cpp         # DeviceConfig validation + JSON
│       ├── runtime_log.h/.cpp          # Daily/hourly heater, pump, filter time (+ TOU peak split)
│       ├── maintenance.h/.cpp          # Filters and recurring tasks (NVS)
│       └── water_log.h/.cpp            # Test strips, chemicals, FROG cartridges
└── data/                               # Uploaded to LittleFS (pio run -t uploadfs)
    ├── index.html
    ├── app.js
    └── style.css
```

### PlatformIO Dependencies (`platformio.ini`)

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
board_build.filesystem = littlefs
monitor_speed = 115200
lib_deps =
  ESP Async WebServer          ; ESPAsyncWebServer
  AsyncTCP                     ; required by ESPAsyncWebServer
  bblanchon/ArduinoJson        ; JSON parsing/serialization
  tzapu/WiFiManager            ; captive portal WiFi config
  knolleary/PubSubClient       ; MQTT (optional)
  ; ArduinoOTA is part of ESP32 Arduino core, no lib needed
upload_protocol = esptool
extra_scripts = pre:scripts/upload_fs.py   ; optional helper
```

### Persistent Files (the `userdata` LittleFS partition, so web uploads don't erase them)

```
/config.json    # timezone, location, filter cycles, TOU + rates, power, tub volume, smart {minTemp, auto, readyTemp}
/runtime.json   # 30 days + 48 hours of runtime totals
/water.json     # water care log
/thermal.json   # last 30 measured heating runs and cool-downs
/smart.json     # active soak, manual hold
/usage.json     # 2 x 48 soak likelihoods + the day being recorded
```
Maintenance lives in NVS instead.

---

## Balboa RS-485 Protocol Notes

- **Baud**: 115200, 8N1
- The spa pack broadcasts a status frame every ~500ms
- The WiFi module joins the bus, acknowledges, and can inject command frames
- **Half-duplex**: Toggle GPIO 4 HIGH before writing, LOW immediately after `Serial2.flush()`

### Key Reference Implementations to Study

- [balboa-spa-client](https://github.com/cdrobey/balboa-spa-client) — Node.js; best protocol docs/comments
- ESPHome `balboa_spa` component — C++ parser, good reference for frame structure
- Search GitHub for `balboa RS485 ESP32` for Arduino implementations

### SpaState Struct (implement in `balboa/spa.h`)

```cpp
struct SpaState {
  float waterTemp;          // current water temp °F
  float setTemp;            // target temp °F
  bool jet1, jet2;
  bool blower, light;
  uint8_t heatMode;         // 0=ready, 1=rest, 2=ready-in-rest
  bool heating, priming;
  bool tempRangeHigh;       // true = 80–104°F, false = 50–80°F
  uint8_t filterCycle1Start;  // hour (0–23)
  uint8_t filterCycle1Dur;    // duration in 30-min increments (check protocol)
  uint8_t filterCycle2Start;
  uint8_t filterCycle2Dur;
  bool filterCycle2Enabled;
  uint8_t backlightTimeout;   // minutes; 0 = never (if pack supports it)
  uint8_t hour, minute;       // spa clock
};
```

---

## Smart Heating

The only lever is the **set temperature** (heat mode is read-only). Lowering it never cools the water; it
just lets the water drift down at its natural rate. Everything below decides what set temp to send.

### Thermal model (`scheduler/thermal_model.cpp`)

Water follows `dT/dt = heaterRate (while heating) − lossCoeff × (water − outside)`.

- `heaterRate` (°F/hr the heater adds before losses) is seeded from heater kW and tub volume
  (`kW × 3412 / (gallons × 8.34)`), then learned from heating runs of at least 20 min and 2°F.
- `lossCoeff` (per hour) is seeded at 0.012, then learned from heater-off runs of at least 3 hours. It
  needs the forecast.
- Runs are timed edge to edge (the moments the whole-degree reading ticks) to get around the spa's 1°F
  resolution. Runs while the tub is in use, plus 30 min after, are skipped (cover probably off).
- Under 5 runs, the seed counts as one run in a mean; from 5 runs on, it's the median of the last 30.
- The spa only reports water temp while its pump runs. Between readings, `SmartHeat` cools the last
  reading by the model.

### Weather (`network/weather_client.cpp`)

The Open-Meteo hourly forecast (`hourly=temperature_2m&timeformat=unixtime&past_days=1&forecast_days=3`)
is fetched hourly on its own FreeRTOS task, with a 5 min retry. It uses **plain HTTP** (TLS would cost
~165 KB of flash) and `useHTTP10(true)`, because chunked replies break `getStream()` parsing. The browser
still fetches its own history for the runtime charts.

### Planner (`scheduler/heat_planner.cpp`)

Dynamic programming over water temp (0.25°F grid) × 15-min steps (up to 36 h). It chooses heat or coast
per step to minimize heater kWh × that step's TOU price (`touPrice`: summer/winter rates, peak per
`tou.cpp`; without rates, peak counts 2×). Each step has a `minTemp` requirement, and missing it costs
$100/°F. Each step also has a `maxTemp` (the spa stops there). Memory stays small: one cost row plus one
bit per (step, temp).

### Usage model (`scheduler/usage_model.cpp`)

Soak likelihood per half hour, for workdays and for weekends + the utility's holidays. Starting values
come from the owner: rarely after midnight, mornings or weekday daytime; weekend daytime sometimes;
evenings likely. Each finished day nudges every slot toward 1 if a soak started there (½ for the
neighbouring slots) or toward 0 if not. Workdays move at a rate of 1/20 per day, weekends at 1/8.

A soak start is any of the following, after 2 quiet hours:
- the use detector turning on;
- a manual set-temp raise;
- a planned soak reaching its ready time.

**Use detector** (`use_detector.cpp`): jets high, jet 2, blower or light, or pump 1 on low for 10+ min
with no filter cycle or heating to explain it.

Likelihood sets how long the owner will wait for heat-up:

| Likelihood | Ready within |
|---|---|
| ≥ 0.25 (likely) | 30 min |
| ≥ 0.04 (possible) | 2 h |
| otherwise | 4 h |

### Controller modes (`scheduler/heat_controller.cpp`)

- **Auto** (Smart Standby, `smart.auto`, on by default): each step's `minTemp` is the readiness temp
  (`readinessTemp`), i.e. the coldest water that still reaches the **comfort temp** within that slot's
  wait. It is never below `smart.minTemp` (90). The comfort temp is the **Ready Temp** setting
  (`smart.readyTemp`, default 102). Turning the spa up by hand (e.g. to 104 for a soak) doesn't change it.
- **Soak**: target temp held from `readyAt` for N hours, with the plan meeting it as cheaply as possible.
  As soon as possible (`readyAt` 0) heats straight through. A soak runs on until 30 min after the last
  use. When it ends, Auto takes over right away, even mid heater run (or the old set temp is restored if Auto
  is off). Ending it by hand (**End**/**Cancel**) means the owner is done, so this happens even with the
  light or jets still on.
- **Manual**: any set temp change the controller didn't send (app or spa panel) pauses everything. The
  pause lasts until 1 h after the last use, or 4 h if the tub is never used. **Resume** ends it early.
- **Low temp range**: the controller does nothing at all (no 90°F floor, any soak dropped), so the tub can
  cool. The range switch itself doesn't count as a manual change, and switching back to high resumes Auto.

Turning the plan into set temps:
- While heating: the highest planned temp in that heating run.
- While coasting: that step's `minTemp`, rounded up (90 overnight). Water cooling faster than planned
  shows up in the next reading, and the replan moves the heating earlier.

Rules:
- Replan every 15 min from the current water temp.
- Never lower the set temp while the tub is in use, except right after the owner ends a soak.
- Do nothing on a stale spa state or before NTP has set the clock.

`SmartHeat` runs this once a second, sends `setTemp` through the spa command queue, and saves
`/smart.json` and `/usage.json`.

---

## WebSocket API (bidirectional JSON)

### ESP32 → Browser

`{"type":"state","data":{...}}` holds the spa state (`SpaController::writeStateJson`), `capabilities`,
and `smart`:
```json
"smart": { "mode": "auto|soak|manual|idle", "comfortTemp": 102, "readyInMin": 45,
           "auto": { "phase": "waiting|heating", "nextHeatAt": 1793051100 },
           "soak": { "target": 104, "readyAt": 1793064600, "hours": 2, "asap": false, "endsAt": 1793071800,
                     "phase": "waiting|heating|ready", "nextHeatAt": 1793048400, "cost": 1.12 },
           "manual": { "since": 1793044745, "temp": 103, "until": 1793059145 },
           "error": "only when a set temp command failed" }
```

### Browser → ESP32 (also `POST /api/cmd`)
```json
{ "action": "setTemp", "value": 103 }
{ "action": "togglePump", "pump": 1 }            { "action": "setPump", "pump": 1, "value": 2 }
{ "action": "light", "value": true }             { "action": "hold", "value": true }
{ "action": "tempRange", "value": "high" }       { "action": "syncTime" }
{ "action": "setFilter1", "startHour": 2, "startMinute": 0, "durationHour": 3, "durationMinute": 0 }
{ "action": "setFilter2", "enabled": true, "startHour": 14, ... }
{ "action": "soak", "target": 104, "readyAt": 1793064600, "hours": 2 }   // readyAt 0/absent = ASAP
{ "action": "cancelSoak" }                       { "action": "resumeSmart" }
```
Also: `readFilterCycles`, `refreshSpaInfo`, `readFaultLog`, `clearReminder`, `start/stop/clearStatusCapture`.

## REST Endpoints

```
GET  /api/state        same as the WebSocket state message
POST /api/cmd          a command (above)
GET  /api/config       POST /api/config (partial updates; only the changed fields)
GET  /api/runtime      daily + hourly runtime
GET  /api/thermal      learned model, measured runs, forecast
GET  /api/smart        smart state + the active plan (with a fair hold-comfort cost comparison) + usage grid
GET  /api/plan         read-only soak preview: ?target=&readyAt=&hours= (plus a "turn it up now" cost)
GET  /api/maintenance  POST /api/maintenance
GET  /api/water        POST /api/water   (FROG readings use sanLevel 1 low / 2 OK / 3 high instead of ppm)
GET  /api/spa-info  /api/faults  /api/system  /api/debug-log  /api/status-capture
```

---

## Web App UI (vanilla HTML/CSS/JS in `data/`)

- **Home**: set temp orb, jets/light/soak buttons, and banners:
  - Auto: "Standby until 3:45 PM · 56m to 102°" with **Heat Now**.
  - Soak: shown with **Cancel**.
  - Manual: shown with **Resume**.
  - Hold and spa reminders.
- **Soak sheet**: temp, "As soon as possible" or a ready time, keep-hot hours, and a live `/api/plan`
  preview (heater windows, peak use, cost vs. turning it up now).
- **Stats**:
  - Overview.
  - Runtime: Next 36 Hours plan chart, When You Soak heatmap, last 48 h, 14 days, energy cost, heating
    model + measured runs.
  - Diagnostics.
- **Maintenance**:
  - Hold mode, with tasks.
  - Filters.
  - Water: FROG @ease logs chlorine by color zone, other systems in ppm.
- **Settings**:
  - General: temp range, Smart Standby, lowest smart temp, clock, location, equipment, security.
  - Schedule: filter cycles, TOU.
  - Energy: rates, power, tub volume.
  - Spa info.

`tools/mock_spa.py` serves `data/` with a simulated spa (including a Python copy of the planner and
smart modes) on :8090 for local testing.

---

## Implementation Order

**Phase 1 — Skeleton**: PlatformIO project, WiFiManager, NTP sync, serial monitor output.

**Phase 2 — Protocol**: RS-485 RX → parse status frames → print decoded `SpaState` to serial. Verify values match physical spa display. Then implement TX: set-temp, toggle jets/lights, set-time, filter cycles, backlight timeout.

**Phase 3 — Web server**: LittleFS serve, WebSocket hub, REST endpoints, `store.h` JSON helpers.

**Phase 4 — Web app**: Dashboard (live state via WebSocket), Settings page, Time sync button.

**Phase 5 — Smart features** (done): forecast fetch, learned thermal model, DP planner, soak/manual/Auto controller, usage learning — see Smart Heating above.

**Phase 6 — Polish**: Heat history UI, MQTT, ArduinoOTA, `uploadfs` OTA.

---

## Verification Checklist

- [ ] Serial monitor shows decoded spa state matching physical display (temp, jets, filter times)
- [ ] Set-temp command from web UI updates spa display within 2s
- [ ] Time sync: after simulated power cut, "Sync" button re-aligns spa clock
- [ ] Filter cycle change in settings page reflects on spa display
- [ ] Schedule for 4 PM ready time shows heat plan with no peak overlap
- [ ] After 10 sessions, `/api/history` shows data and rate values correlate with ambient
- [ ] Two browser tabs both update when a control is toggled (WebSocket broadcast)
- [ ] OTA firmware update completes over WiFi without USB cable
