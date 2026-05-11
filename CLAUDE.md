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
| Ambient temp | Open-Meteo API (free, no key) | Lat/lon only, tiny response |
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
│   │   ├── web_server.h/.cpp           # ESPAsyncWebServer + WebSocket hub
│   │   ├── weather_client.h/.cpp       # Open-Meteo current temp fetch
│   │   └── mqtt.h/.cpp                 # Optional HA MQTT bridge
│   ├── scheduler/
│   │   ├── session_scheduler.h/.cpp    # TOU-aware pre-heat planner
│   │   └── heat_model.h/.cpp           # Heat rate model + linear regression
│   └── storage/
│       └── store.h/.cpp                # LittleFS JSON read/write helpers
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

### Persistent Files in LittleFS

```
/config.json       # lat, lon, tou_start (17), tou_end (21), mqtt settings, backlight_timeout
/schedules.json    # array of scheduled sessions
/heat_history.json # last 200 heat sessions — prune oldest on write
```

`heat_history.json` record schema:
```json
{ "ts": 1715000000, "ambient": 58.0, "t_start": 95.5, "t_end": 102.0, "dur_min": 87 }
```

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

## Smart Features

### TOU-Aware Pre-heat Scheduler

Peak electricity window: **5:00 PM – 9:00 PM** (configurable in `/config.json` as `tou_start`/`tou_end` in 24h hours).

**Algorithm** (implement in `scheduler/session_scheduler.cpp`):

```
heatingRate  = heatModel.predict(ambientTemp, currentWaterTemp)   // °F/hr
hoursNeeded  = (targetTemp - currentWaterTemp) / heatingRate
idealStart   = targetReadyTime - hoursNeeded

if idealStart + hoursNeeded <= peakStart:
    → schedule single heat window starting at idealStart

else if targetReadyTime > peakEnd:
    → heat from idealStart until peakStart, pause, resume at peakEnd

else (target falls inside peak, unavoidable):
    → start at (targetReadyTime - hoursNeeded), heat straight through
    → flag in UI: "⚡ Peak overlap unavoidable"
```

Schedules are stored in `/schedules.json`. The main loop checks every minute and sends set-temp / heat-mode commands at the planned times.

### Heat Rate Model (implement in `scheduler/heat_model.cpp`)

**Baseline** (used until ≥10 data points):
```
rate (°F/hr) = 1.0 + 0.035 * ambientTemp
```
Examples: 40°F → ~2.4°F/hr, 65°F → ~3.3°F/hr, 80°F → ~3.8°F/hr

**Learning**: After each completed heat session, log a data point to `heat_history.json`. Once 10+ points exist, fit a linear regression:
```
rate = a + b * ambient
```
Use simple least-squares. Store `a` and `b` in `/config.json`. This silently improves over time.

After a heat session completes (set-temp was commanded → water reached target), record the data point automatically.

### Weather (implement in `network/weather_client.cpp`)

Open-Meteo endpoint (no API key needed):
```
https://api.open-meteo.com/v1/forecast
  ?latitude={lat}
  &longitude={lon}
  &current=temperature_2m
  &temperature_unit=fahrenheit
  &forecast_days=1
```

Parse `current.temperature_2m` from the JSON response. Fetch every 30 minutes. Cache the last reading for scheduling calculations. Lat/lon stored in `/config.json`.

---

## WebSocket API (bidirectional JSON)

### ESP32 → Browser (push on every state change, ~500ms)
```json
{
  "type": "state",
  "data": {
    "waterTemp": 101.5, "setTemp": 102,
    "jet1": true, "jet2": false,
    "light": true, "heating": true, "heatMode": 0,
    "filterCycle1": { "start": 6, "dur": 120 },
    "filterCycle2": { "start": 18, "dur": 60, "enabled": true },
    "backlightTimeout": 5,
    "ambientTemp": 58.0,
    "spaTime": "14:32",
    "schedule": { "active": true, "readyAt": 1715120400, "targetTemp": 102,
                  "heatWindows": [{"start": 1715112600, "end": 1715114400}] }
  }
}
```

### Browser → ESP32 (commands)
```json
{ "type": "cmd", "action": "setTemp",          "value": 103 }
{ "type": "cmd", "action": "jet1",             "value": true }
{ "type": "cmd", "action": "jet2",             "value": false }
{ "type": "cmd", "action": "light",            "value": true }
{ "type": "cmd", "action": "heatMode",         "value": 0 }
{ "type": "cmd", "action": "filterCycle1",     "start": 6, "dur": 120 }
{ "type": "cmd", "action": "filterCycle2",     "start": 18, "dur": 60, "enabled": true }
{ "type": "cmd", "action": "backlightTimeout", "value": 5 }
{ "type": "cmd", "action": "syncTime" }
{ "type": "cmd", "action": "scheduleSession",  "readyAt": 1715120400, "targetTemp": 102 }
{ "type": "cmd", "action": "cancelSchedule" }
```

## REST Endpoints

```
GET  /api/state      → full SpaState JSON
POST /api/cmd        → same body as WebSocket command
GET  /api/schedule   → current + upcoming sessions
GET  /api/history    → heat session log
GET  /api/config     → all config values
POST /api/config     → save config (lat/lon, TOU hours, MQTT, backlight)
```

---

## Web App UI (vanilla HTML/CSS/JS in `data/`)

### Dashboard view
- Current water temp + target temp with +/- buttons
- Heating status indicator
- Jet 1, Jet 2, Lights toggle buttons
- Heat mode selector (Ready / Rest / Ready-in-Rest)
- Spa time display + "Sync Time" button (triggers NTP → spa clock set)
- "Schedule Session" button

### Schedule Session view
- Date + time picker ("Ready at:")
- Target temp input (defaults to current set temp)
- Plan preview: shows computed heat windows, flags peak overlap
- Save / Cancel

### History view
- Table: date, ambient, start temp, end temp, duration, computed rate
- Simple canvas chart of rate vs ambient temp (helps validate the model)

### Settings view
- TOU peak start/end hours
- Filter cycle 1: start hour, duration (minutes)
- Filter cycle 2: start hour, duration, enable toggle
- Backlight timeout (minutes; 0 = always on)
- Lat/lon for weather
- MQTT: broker host, port, topic prefix, username/password (off by default)
- "Reconfigure WiFi" button (triggers WiFiManager reset)

---

## Implementation Order

**Phase 1 — Skeleton**: PlatformIO project, WiFiManager, NTP sync, serial monitor output.

**Phase 2 — Protocol**: RS-485 RX → parse status frames → print decoded `SpaState` to serial. Verify values match physical spa display. Then implement TX: set-temp, toggle jets/lights, set-time, filter cycles, backlight timeout.

**Phase 3 — Web server**: LittleFS serve, WebSocket hub, REST endpoints, `store.h` JSON helpers.

**Phase 4 — Web app**: Dashboard (live state via WebSocket), Settings page, Time sync button.

**Phase 5 — Smart features**: Open-Meteo fetch, heat model (baseline then regression), TOU scheduler, Schedule Session UI with plan preview, main-loop schedule executor.

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
