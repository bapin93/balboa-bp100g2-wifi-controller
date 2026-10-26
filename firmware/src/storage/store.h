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
  void loadRuntime(RuntimeLog &log);
  bool saveRuntime(const RuntimeLog &log);
  // Kept in NVS rather than LittleFS so web-file and firmware uploads don't erase it.
  void loadMaintenance(MaintenanceLog &log);
  bool saveMaintenance(MaintenanceLog &log);
  // Boots in a row that never joined WiFi; enough of them reopens the setup portal.
  uint8_t loadWifiFailedBoots();
  void saveWifiFailedBoots(uint8_t count);

 private:
  bool ensureDefaults();
};
