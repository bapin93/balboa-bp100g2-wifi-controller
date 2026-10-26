#include "network/weather_client.h"

#include <HTTPClient.h>
#include <WiFi.h>

#include <ctime>

namespace {

constexpr uint32_t RefreshMs = 60UL * 60UL * 1000UL;
constexpr uint32_t RetryMs = 5UL * 60UL * 1000UL;
constexpr uint32_t PollMs = 5000;
constexpr uint32_t ClockValidAfter = 1700000000;
constexpr uint32_t ShowPastSec = 6 * 3600;
constexpr uint32_t ShowAheadSec = 36 * 3600;

}  // namespace

void WeatherClient::begin() {
  xTaskCreate(taskMain, "weather", 12288, this, 1, nullptr);
}

void WeatherClient::setLocation(bool hasLocation, float latitude, float longitude) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (hasLocation == hasLocation_ && latitude == latitude_ && longitude == longitude_) {
    return;
  }
  hasLocation_ = hasLocation;
  latitude_ = latitude;
  longitude_ = longitude;
  ++locationSeq_;
  if (!hasLocation) {
    hours_.clear();
    fetchedAt_ = 0;
  }
}

float WeatherClient::outsideAt(uint32_t epoch) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (hours_.empty() || epoch < hours_.front().first || epoch > hours_.back().first + 3600) {
    return NAN;
  }
  for (size_t i = 1; i < hours_.size(); ++i) {
    if (epoch <= hours_[i].first) {
      const auto &a = hours_[i - 1];
      const auto &b = hours_[i];
      const float t = static_cast<float>(epoch - a.first) / static_cast<float>(b.first - a.first);
      return a.second + (b.second - a.second) * t;
    }
  }
  return hours_.back().second;
}

void WeatherClient::writeJson(JsonObject out, uint32_t now) const {
  const float current = outsideAt(now);
  std::lock_guard<std::mutex> lock(mutex_);
  out["hasLocation"] = hasLocation_;
  if (fetchedAt_) {
    out["fetchedAt"] = fetchedAt_;
  } else {
    out["fetchedAt"] = nullptr;
  }
  if (lastError_[0]) {
    out["error"] = lastError_;
  }
  if (!isnan(current)) {
    out["now"] = roundf(current * 10.0f) / 10.0f;
  }
  JsonArray hours = out["hours"].to<JsonArray>();
  for (const auto &hour : hours_) {
    if (hour.first + ShowPastSec >= now && hour.first <= now + ShowAheadSec) {
      JsonObject item = hours.add<JsonObject>();
      item["t"] = hour.first;
      item["temp"] = hour.second;
    }
  }
}

void WeatherClient::taskMain(void *arg) {
  static_cast<WeatherClient *>(arg)->runTask();
}

void WeatherClient::runTask() {
  uint32_t lastAttemptMs = 0;
  uint32_t fetchedSeq = 0;
  bool attempted = false;
  bool lastOk = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(PollMs));
    bool hasLocation;
    float latitude;
    float longitude;
    uint32_t seq;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      hasLocation = hasLocation_;
      latitude = latitude_;
      longitude = longitude_;
      seq = locationSeq_;
    }
    if (!hasLocation || WiFi.status() != WL_CONNECTED || time(nullptr) < ClockValidAfter) {
      continue;
    }
    const uint32_t now = millis();
    const bool due = !attempted || seq != fetchedSeq || now - lastAttemptMs >= (lastOk ? RefreshMs : RetryMs);
    if (!due) {
      continue;
    }
    attempted = true;
    lastAttemptMs = now;
    fetchedSeq = seq;
    lastOk = fetch(latitude, longitude);
  }
}

bool WeatherClient::fetch(float latitude, float longitude) {
  char url[256];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&hourly=temperature_2m"
           "&temperature_unit=fahrenheit&timeformat=unixtime&past_days=1&forecast_days=3",
           latitude, longitude);
  // Plain HTTP: the forecast isn't sensitive, and TLS would cost about 165 KB of flash.
  WiFiClient client;
  HTTPClient http;
  http.useHTTP10(true);  // HTTP/1.1 replies come chunked, and getStream() hands back the raw chunk markers
  http.setTimeout(10000);
  http.setConnectTimeout(10000);
  const auto fail = [this](const char *format, int detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    snprintf(lastError_, sizeof(lastError_), format, detail);
    Serial.printf("[weather] %s\n", lastError_);
    return false;
  };
  if (!http.begin(client, url)) {
    return fail("couldn't start request (%d)", 0);
  }
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    http.end();
    return fail("Open-Meteo answered %d", status);
  }
  JsonDocument filter;
  filter["hourly"]["time"] = true;
  filter["hourly"]["temperature_2m"] = true;
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (error) {
    return fail("couldn't read forecast (%d)", static_cast<int>(error.code()));
  }
  if (!doc["hourly"].is<JsonObjectConst>()) {
    return fail("forecast had no hourly data (%d)", 0);
  }
  JsonArrayConst times = doc["hourly"]["time"];
  JsonArrayConst temps = doc["hourly"]["temperature_2m"];
  std::vector<std::pair<uint32_t, float>> hours;
  hours.reserve(times.size());
  for (size_t i = 0; i < times.size() && i < temps.size(); ++i) {
    if (temps[i].is<float>()) {
      hours.emplace_back(times[i].as<uint32_t>(), temps[i].as<float>());
    }
  }
  if (hours.size() < 2) {
    return fail("forecast had %d hours", static_cast<int>(hours.size()));
  }
  std::lock_guard<std::mutex> lock(mutex_);
  hours_ = std::move(hours);
  fetchedAt_ = static_cast<uint32_t>(time(nullptr));
  lastError_[0] = '\0';
  Serial.printf("[weather] %u hours, heap free %u\n", static_cast<unsigned>(hours_.size()), ESP.getFreeHeap());
  return true;
}
