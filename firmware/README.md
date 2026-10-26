# BP100G2 Manual Controller Firmware

This is the v1 reliable manual controller scaffold. It intentionally excludes smart scheduling, weather, MQTT, heat learning, and history UI until the RS-485 controller is validated against the physical spa.

## Build

```sh
pio run -d firmware
pio test -d firmware -e native
pio run -d firmware -t upload
pio run -d firmware -t uploadfs
```

## Web App Development With a Mock Spa

`tools/mock_spa.py` serves `data/` uncached and fakes the firmware's API and WebSocket with a simulated
spa (heating, filter cycles, pump 1 held on while heating, hold countdown, reminders, fault log, runtime
history). Edit files in `data/`, then refresh the browser. Python 3 standard library only.

```sh
python3 firmware/tools/mock_spa.py                    # http://127.0.0.1:8090
python3 firmware/tools/mock_spa.py --host 0.0.0.0     # open from your phone on the same WiFi
python3 firmware/tools/mock_spa.py --help             # pumps, starting temp, reminder, fault log options
```

Force any state while it runs:

```sh
curl -X POST localhost:8090/mock -d '{"reminder": 10}'      # check pH banner
curl -X POST localhost:8090/mock -d '{"hold": true, "holdMinutes": 5}'
curl -X POST localhost:8090/mock -d '{"pumps": "2,1"}'      # add a single-speed Jet 2
curl -X POST localhost:8090/mock -d '{"waterTemp": 80}'
```

The mock mirrors the JSON in `src/balboa/spa.cpp`; update both when the API changes. When the app looks
right, deploy with `pio run -e esp32dev_ota -t uploadfs`.

## First Hardware Bring-Up

1. Power the ESP32 from USB with RS-485 disconnected and verify WiFi setup.
2. Upload LittleFS and confirm the Dashboard loads.
3. Connect RS-485 receive-only first if your transceiver wiring allows it.
4. Watch serial logs for `[balboa] rx` frames and parser errors.
5. Confirm decoded state against the physical panel before sending commands.

Command button codes are intentionally isolated in `src/balboa/spa.cpp` so they can be adjusted after captured-frame validation for the exact pack/panel.
