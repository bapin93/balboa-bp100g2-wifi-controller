#include "storage/maintenance.h"

#include <string.h>

namespace {

std::string trimmed(const std::string &text) {
  const size_t first = text.find_first_not_of(" \t");
  if (first == std::string::npos) {
    return "";
  }
  const size_t last = text.find_last_not_of(" \t");
  return text.substr(first, last - first + 1);
}

const char *validateName(const std::string &name) {
  if (name.empty()) {
    return "give the filter a name";
  }
  if (name.size() > MaintenanceLog::MaxNameLength) {
    return "name is too long (24 characters max)";
  }
  return nullptr;
}

const char *validateInterval(int days) {
  if (days < 1 || days > MaintenanceLog::MaxIntervalDays) {
    return "repeat every 1 to 730 days";
  }
  return nullptr;
}

MaintenanceEventType eventTypeFromName(const char *name) {
  const MaintenanceEventType types[] = {MaintenanceEventType::Added, MaintenanceEventType::Installed,
                                        MaintenanceEventType::Removed, MaintenanceEventType::Cleaned,
                                        MaintenanceEventType::Deleted, MaintenanceEventType::Done};
  for (MaintenanceEventType type : types) {
    if (strcmp(name, maintenanceEventName(type)) == 0) {
      return type;
    }
  }
  return MaintenanceEventType::Added;
}

}  // namespace

const char *maintenanceEventName(MaintenanceEventType type) {
  switch (type) {
    case MaintenanceEventType::Added:
      return "added";
    case MaintenanceEventType::Installed:
      return "installed";
    case MaintenanceEventType::Removed:
      return "removed";
    case MaintenanceEventType::Cleaned:
      return "cleaned";
    case MaintenanceEventType::Deleted:
      return "deleted";
    case MaintenanceEventType::Done:
      return "done";
  }
  return "added";
}

FilterRecord *MaintenanceLog::findFilter(uint16_t id) {
  for (FilterRecord &filter : filters_) {
    if (filter.id == id) {
      return &filter;
    }
  }
  return nullptr;
}

MaintenanceTask *MaintenanceLog::findTask(uint16_t id) {
  for (MaintenanceTask &task : tasks_) {
    if (task.id == id) {
      return &task;
    }
  }
  return nullptr;
}

void MaintenanceLog::record(uint32_t ts, uint16_t id, const std::string &name, MaintenanceEventType type) {
  MaintenanceEvent event;
  event.ts = ts;
  event.itemId = id;
  event.name = name;
  event.type = type;
  events_.push_back(event);
  if (events_.size() > MaxEvents) {
    events_.erase(events_.begin());
  }
  dirty_ = true;
}

const char *MaintenanceLog::addFilter(const std::string &rawName, uint32_t now, uint16_t *id) {
  const std::string name = trimmed(rawName);
  if (const char *error = validateName(name)) {
    return error;
  }
  if (filters_.size() >= MaxFilters) {
    return "too many filters; delete one first";
  }
  FilterRecord filter;
  filter.id = nextId_++;
  filter.name = name;
  filter.createdAt = now;
  filters_.push_back(filter);
  record(now, filters_.back().id, filters_.back().name, MaintenanceEventType::Added);
  if (id) {
    *id = filter.id;
  }
  return nullptr;
}

const char *MaintenanceLog::renameFilter(uint16_t id, const std::string &rawName) {
  FilterRecord *filter = findFilter(id);
  if (!filter) {
    return "filter not found";
  }
  const std::string name = trimmed(rawName);
  if (const char *error = validateName(name)) {
    return error;
  }
  filter->name = name;
  dirty_ = true;
  return nullptr;
}

const char *MaintenanceLog::deleteFilter(uint16_t id, uint32_t now) {
  for (size_t i = 0; i < filters_.size(); ++i) {
    if (filters_[i].id == id) {
      record(now, filters_[i].id, filters_[i].name, MaintenanceEventType::Deleted);
      filters_.erase(filters_.begin() + static_cast<long>(i));
      return nullptr;
    }
  }
  return "filter not found";
}

const char *MaintenanceLog::install(uint16_t id, uint32_t ts) {
  FilterRecord *filter = findFilter(id);
  if (!filter) {
    return "filter not found";
  }
  if (filter->inTub()) {
    return "that filter is already in the tub";
  }
  if (!filter->stints.empty() && ts < filter->stints.back().out) {
    return "date is before this filter was last taken out";
  }
  if (filter->stints.size() >= MaxStints) {
    // Years of swaps: merge the two oldest stints into one, keeping their combined length so the
    // lifetime in-tub total stays right.
    const uint32_t secondLength = filter->stints[1].out - filter->stints[1].in;
    filter->stints[0].out += secondLength;
    filter->stints.erase(filter->stints.begin() + 1);
  }
  filter->stints.push_back({ts, 0});
  record(ts, filter->id, filter->name, MaintenanceEventType::Installed);
  return nullptr;
}

const char *MaintenanceLog::takeOut(uint16_t id, uint32_t ts) {
  FilterRecord *filter = findFilter(id);
  if (!filter) {
    return "filter not found";
  }
  if (!filter->inTub()) {
    return "that filter is not in the tub";
  }
  if (ts < filter->stints.back().in) {
    return "date is before this filter went in";
  }
  filter->stints.back().out = ts;
  record(ts, filter->id, filter->name, MaintenanceEventType::Removed);
  return nullptr;
}

const char *MaintenanceLog::swap(uint16_t outId, uint16_t inId, uint32_t ts) {
  FilterRecord *out = findFilter(outId);
  FilterRecord *in = findFilter(inId);
  if (!out || !in) {
    return "filter not found";
  }
  if (!out->inTub()) {
    return "the filter coming out is not in the tub";
  }
  if (in->inTub()) {
    return "the filter going in is already in the tub";
  }
  if (ts < out->stints.back().in || (!in->stints.empty() && ts < in->stints.back().out)) {
    return "date conflicts with earlier history";
  }
  takeOut(outId, ts);
  return install(inId, ts);
}

const char *MaintenanceLog::clean(uint16_t id, uint32_t ts) {
  FilterRecord *filter = findFilter(id);
  if (!filter) {
    return "filter not found";
  }
  if (ts < filter->createdAt) {
    return "date is before the filter was added";
  }
  filter->cleanedAt = ts;
  filter->pumpSecSinceClean = 0;
  record(ts, filter->id, filter->name, MaintenanceEventType::Cleaned);
  return nullptr;
}

const char *MaintenanceLog::addTask(const std::string &rawName, int intervalDays, uint32_t now, uint16_t *id) {
  const std::string name = trimmed(rawName);
  if (const char *error = validateName(name)) {
    return error;
  }
  if (const char *error = validateInterval(intervalDays)) {
    return error;
  }
  if (tasks_.size() >= MaxTasks) {
    return "too many tasks; delete one first";
  }
  MaintenanceTask task;
  task.id = nextId_++;
  task.name = name;
  task.intervalDays = static_cast<uint16_t>(intervalDays);
  task.createdAt = now;
  tasks_.push_back(task);
  dirty_ = true;
  if (id) {
    *id = task.id;
  }
  return nullptr;
}

const char *MaintenanceLog::editTask(uint16_t id, const std::string &rawName, int intervalDays) {
  MaintenanceTask *task = findTask(id);
  if (!task) {
    return "task not found";
  }
  const std::string name = trimmed(rawName);
  if (const char *error = validateName(name)) {
    return error;
  }
  if (const char *error = validateInterval(intervalDays)) {
    return error;
  }
  task->name = name;
  task->intervalDays = static_cast<uint16_t>(intervalDays);
  dirty_ = true;
  return nullptr;
}

const char *MaintenanceLog::deleteTask(uint16_t id) {
  for (size_t i = 0; i < tasks_.size(); ++i) {
    if (tasks_[i].id == id) {
      tasks_.erase(tasks_.begin() + static_cast<long>(i));
      dirty_ = true;
      return nullptr;
    }
  }
  return "task not found";
}

const char *MaintenanceLog::completeTask(uint16_t id, uint32_t ts) {
  MaintenanceTask *task = findTask(id);
  if (!task) {
    return "task not found";
  }
  // Recording an older completion after a newer one keeps the newer date as "last done".
  if (ts > task->lastDone) {
    task->lastDone = ts;
  }
  record(ts, task->id, task->name, MaintenanceEventType::Done);
  return nullptr;
}

void MaintenanceLog::ensureDefaultTasks(uint32_t now) {
  if (tasksSeeded_) {
    return;
  }
  addTask("Drain and refill", 90, now);
  addTask("Clean the cover", 30, now);
  addTask("Deep-clean filters", 90, now);
  tasksSeeded_ = true;
  dirty_ = true;
}

void MaintenanceLog::addPumpTime(uint32_t seconds) {
  for (FilterRecord &filter : filters_) {
    if (filter.inTub()) {
      filter.pumpSec += seconds;
      filter.pumpSecSinceClean += seconds;
      dirty_ = true;
    }
  }
}

void MaintenanceLog::toJson(JsonObject out) const {
  out["nextId"] = nextId_;
  JsonArray filters = out["filters"].to<JsonArray>();
  for (const FilterRecord &filter : filters_) {
    JsonObject item = filters.add<JsonObject>();
    item["id"] = filter.id;
    item["name"] = filter.name;
    item["createdAt"] = filter.createdAt;
    item["cleanedAt"] = filter.cleanedAt;
    item["pumpSec"] = filter.pumpSec;
    item["pumpSecSinceClean"] = filter.pumpSecSinceClean;
    item["inTub"] = filter.inTub();
    JsonArray stints = item["stints"].to<JsonArray>();
    for (const FilterStint &stint : filter.stints) {
      JsonArray pair = stints.add<JsonArray>();
      pair.add(stint.in);
      pair.add(stint.out);
    }
  }
  JsonArray tasks = out["tasks"].to<JsonArray>();
  for (const MaintenanceTask &task : tasks_) {
    JsonObject item = tasks.add<JsonObject>();
    item["id"] = task.id;
    item["name"] = task.name;
    item["intervalDays"] = task.intervalDays;
    item["createdAt"] = task.createdAt;
    item["lastDone"] = task.lastDone;
  }
  out["tasksSeeded"] = tasksSeeded_;
  JsonArray events = out["events"].to<JsonArray>();
  for (const MaintenanceEvent &event : events_) {
    JsonObject item = events.add<JsonObject>();
    item["ts"] = event.ts;
    item["itemId"] = event.itemId;
    item["name"] = event.name;
    item["type"] = maintenanceEventName(event.type);
  }
}

void MaintenanceLog::fromJson(JsonVariantConst in) {
  filters_.clear();
  tasks_.clear();
  events_.clear();
  nextId_ = in["nextId"] | 1;
  for (JsonObjectConst item : in["filters"].as<JsonArrayConst>()) {
    if (filters_.size() >= MaxFilters) {
      break;
    }
    FilterRecord filter;
    filter.id = item["id"] | 0;
    filter.name = item["name"] | "";
    filter.createdAt = item["createdAt"] | 0;
    filter.cleanedAt = item["cleanedAt"] | 0;
    filter.pumpSec = item["pumpSec"] | 0;
    filter.pumpSecSinceClean = item["pumpSecSinceClean"] | 0;
    for (JsonArrayConst pair : item["stints"].as<JsonArrayConst>()) {
      if (filter.stints.size() < MaxStints) {
        filter.stints.push_back({pair[0] | 0u, pair[1] | 0u});
      }
    }
    if (filter.id != 0) {
      if (filter.id >= nextId_) {
        nextId_ = filter.id + 1;
      }
      filters_.push_back(filter);
    }
  }
  for (JsonObjectConst item : in["tasks"].as<JsonArrayConst>()) {
    if (tasks_.size() >= MaxTasks) {
      break;
    }
    MaintenanceTask task;
    task.id = item["id"] | 0;
    task.name = item["name"] | "";
    task.intervalDays = item["intervalDays"] | 0;
    task.createdAt = item["createdAt"] | 0;
    task.lastDone = item["lastDone"] | 0;
    if (task.id != 0) {
      if (task.id >= nextId_) {
        nextId_ = task.id + 1;
      }
      tasks_.push_back(task);
    }
  }
  tasksSeeded_ = in["tasksSeeded"] | false;
  for (JsonObjectConst item : in["events"].as<JsonArrayConst>()) {
    if (events_.size() >= MaxEvents) {
      break;
    }
    MaintenanceEvent event;
    event.ts = item["ts"] | 0;
    event.itemId = item["itemId"] | 0;
    event.name = item["name"] | "";
    event.type = eventTypeFromName(item["type"] | "added");
    events_.push_back(event);
  }
  dirty_ = false;
}
