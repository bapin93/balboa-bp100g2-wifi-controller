#pragma once

#include <Arduino.h>

namespace AppConfig {

constexpr uint32_t SerialBaud = 115200;
constexpr uint32_t SpaBaud = 115200;

constexpr int Rs485RxPin = 16;
constexpr int Rs485TxPin = 17;
constexpr int Rs485TxEnablePin = 4;

constexpr const char *WifiApName = "BP100G2-Setup";
constexpr const char *Hostname = "bp100g2-controller";

constexpr const char *ConfigPath = "/config.json";
constexpr size_t MaxCommandQueue = 8;
constexpr uint32_t CommandTimeoutMs = 2500;
constexpr uint32_t StateBroadcastMinMs = 250;
constexpr uint32_t StateStaleMs = 5000;

}  // namespace AppConfig
