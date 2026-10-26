#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include <ArduinoJson.h>

// One stretch of time a filter spent in the tub. out == 0 means it is still in.
struct FilterStint {
  uint32_t in = 0;
  uint32_t out = 0;
};

struct FilterRecord {
  uint16_t id = 0;
  std::string name;
  uint32_t createdAt = 0;
  uint32_t cleanedAt = 0;          // 0 = never cleaned
  uint32_t pumpSec = 0;            // water moving through it, lifetime
  uint32_t pumpSecSinceClean = 0;
  std::vector<FilterStint> stints;

  bool inTub() const { return !stints.empty() && stints.back().out == 0; }
};

// A chore that repeats every intervalDays, e.g. draining and refilling the tub.
struct MaintenanceTask {
  uint16_t id = 0;
  std::string name;
  uint16_t intervalDays = 0;
  uint32_t createdAt = 0;
  uint32_t lastDone = 0;  // 0 = never done
};

enum class MaintenanceEventType : uint8_t {
  Added,
  Installed,
  Removed,
  Cleaned,
  Deleted,
  Done,
};

struct MaintenanceEvent {
  uint32_t ts = 0;
  uint16_t itemId = 0;
  std::string name;  // kept so history still reads well after the item is deleted
  MaintenanceEventType type = MaintenanceEventType::Added;
};

// Filter inventory, recurring tasks and service history. Times are Unix seconds. Methods return nullptr
// on success or an error message for the user.
class MaintenanceLog {
 public:
  static constexpr size_t MaxFilters = 8;
  static constexpr size_t MaxStints = 24;
  static constexpr size_t MaxTasks = 12;
  static constexpr size_t MaxEvents = 40;
  static constexpr size_t MaxNameLength = 24;
  static constexpr uint16_t MaxIntervalDays = 730;

  const char *addFilter(const std::string &name, uint32_t now, uint16_t *id = nullptr);
  const char *renameFilter(uint16_t id, const std::string &name);
  const char *deleteFilter(uint16_t id, uint32_t now);
  const char *install(uint16_t id, uint32_t ts);
  const char *takeOut(uint16_t id, uint32_t ts);
  const char *swap(uint16_t outId, uint16_t inId, uint32_t ts);
  const char *clean(uint16_t id, uint32_t ts);

  const char *addTask(const std::string &name, int intervalDays, uint32_t now, uint16_t *id = nullptr);
  const char *editTask(uint16_t id, const std::string &name, int intervalDays);
  const char *deleteTask(uint16_t id);
  const char *completeTask(uint16_t id, uint32_t ts);
  // Adds the starter tasks once; deleting them later doesn't bring them back.
  void ensureDefaultTasks(uint32_t now);

  // Credits pump run time to every filter in the tub.
  void addPumpTime(uint32_t seconds);

  void toJson(JsonObject out) const;
  void fromJson(JsonVariantConst in);

  const std::vector<FilterRecord> &filters() const { return filters_; }
  const std::vector<MaintenanceTask> &tasks() const { return tasks_; }
  const std::vector<MaintenanceEvent> &events() const { return events_; }
  bool dirty() const { return dirty_; }
  void clearDirty() { dirty_ = false; }

 private:
  FilterRecord *findFilter(uint16_t id);
  MaintenanceTask *findTask(uint16_t id);
  void record(uint32_t ts, uint16_t id, const std::string &name, MaintenanceEventType type);

  std::vector<FilterRecord> filters_;
  std::vector<MaintenanceTask> tasks_;
  std::vector<MaintenanceEvent> events_;
  uint16_t nextId_ = 1;
  bool tasksSeeded_ = false;
  bool dirty_ = false;
};

const char *maintenanceEventName(MaintenanceEventType type);
