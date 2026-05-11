#pragma once

#include <Arduino.h>

#include "storage/config_model.h"

class Store {
 public:
  bool begin();
  DeviceConfig loadConfig();
  bool saveConfig(const DeviceConfig &config);

 private:
  bool ensureDefaults();
};
