# BP100G2 Manual Controller Firmware

This is the v1 reliable manual controller scaffold. It intentionally excludes smart scheduling, weather, MQTT, heat learning, and history UI until the RS-485 controller is validated against the physical spa.

## Build

```sh
pio run -d firmware
pio test -d firmware -e native
pio run -d firmware -t upload
pio run -d firmware -t uploadfs
```

## First Hardware Bring-Up

1. Power the ESP32 from USB with RS-485 disconnected and verify WiFi setup.
2. Upload LittleFS and confirm the Dashboard loads.
3. Connect RS-485 receive-only first if your transceiver wiring allows it.
4. Watch serial logs for `[balboa] rx` frames and parser errors.
5. Confirm decoded state against the physical panel before sending commands.

Command button codes are intentionally isolated in `src/balboa/spa.cpp` so they can be adjusted after captured-frame validation for the exact pack/panel.
