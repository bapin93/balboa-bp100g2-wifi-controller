#pragma once

#include <Arduino.h>

#include <vector>

#include "storage/config_model.h"
#include "storage/maintenance.h"
#include "storage/runtime_log.h"

class Store {
 public:
  bool begin();
  DeviceConfig loadConfig();
  bool saveConfig(const DeviceConfig &config);
  std::vector<RuntimeDay> loadRuntime();
  bool saveRuntime(const RuntimeLog &log);
  // Kept in NVS rather than LittleFS so web-file and firmware uploads don't erase it.
  void loadMaintenance(MaintenanceLog &log);
  bool saveMaintenance(MaintenanceLog &log);

 private:
  bool ensureDefaults();
};
