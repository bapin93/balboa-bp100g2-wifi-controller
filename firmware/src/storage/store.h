#pragma once

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>

#include <vector>

#include "storage/config_model.h"
#include "storage/maintenance.h"
#include "storage/runtime_log.h"

class Store {
 public:
  bool begin();
  // True when settings and history are on their own partition (false = fallback onto the web partition).
  bool hasDataPartition() const { return data_ == &dataFs_; }
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

  fs::LittleFSFS dataFs_;
  fs::FS *data_ = &LittleFS;  // where settings and runtime history live
};
