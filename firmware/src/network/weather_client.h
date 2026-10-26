#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <mutex>
#include <utility>
#include <vector>

// Hourly outside temperature from Open-Meteo (free, no key), for the heating model and planner.
// Fetching runs on its own task: a slow DNS lookup or server can take longer than loop()'s watchdog allows.
class WeatherClient {
 public:
  void begin();
  // Called at boot and whenever the location setting changes; a change refetches right away.
  void setLocation(bool hasLocation, float latitude, float longitude);
  // °F at `epoch`, interpolated between hours; NAN when the forecast doesn't cover it.
  float outsideAt(uint32_t epoch) const;
  void writeJson(JsonObject out, uint32_t now) const;

 private:
  static void taskMain(void *arg);
  void runTask();
  bool fetch(float latitude, float longitude);

  mutable std::mutex mutex_;
  std::vector<std::pair<uint32_t, float>> hours_;  // (Unix time at the top of the hour, °F), oldest first
  bool hasLocation_ = false;
  float latitude_ = 0;
  float longitude_ = 0;
  uint32_t locationSeq_ = 0;
  uint32_t fetchedAt_ = 0;  // Unix time of the last good fetch
  char lastError_[64] = "";
};
