#pragma once

#include <Arduino.h>

namespace AppConfig {

constexpr uint32_t SerialBaud = 115200;
constexpr uint32_t SpaBaud = 115200;

constexpr int Rs485RxPin = 16;
constexpr int Rs485TxPin = 17;
constexpr int Rs485TxEnablePin = 4;

constexpr const char *WifiApName = "BP100G2-Setup";
constexpr uint32_t WifiRetryIntervalMs = 30UL * 1000UL;
constexpr uint32_t WifiRestartAfterMs = 15UL * 60UL * 1000UL;  // last resort when WiFi stays down
constexpr uint8_t WifiPortalAfterFailedBoots = 3;            // then assume the network changed
constexpr const char *Hostname = "bp100g2-controller";

constexpr const char *DataPartitionLabel = "userdata";
constexpr const char *ConfigPath = "/config.json";
constexpr const char *RuntimePath = "/runtime.json";
constexpr const char *WaterPath = "/water.json";
constexpr const char *ThermalPath = "/thermal.json";
constexpr uint32_t RuntimeSaveIntervalMs = 15UL * 60UL * 1000UL;
constexpr size_t MaxCommandQueue = 8;
constexpr uint32_t StateBroadcastMinMs = 250;

}  // namespace AppConfig
