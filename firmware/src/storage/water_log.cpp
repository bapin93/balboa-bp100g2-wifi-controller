#include "storage/water_log.h"

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

// Test strips read to a tenth at best; rounding keeps the saved JSON short.
float rounded(float value) {
  return isnan(value) ? NAN : roundf(value * 100.0f) / 100.0f;
}

bool outside(float value, float min, float max) {
  return !isnan(value) && (value < min || value > max);
}

const char *entryTypeName(WaterEntryType type) {
  switch (type) {
    case WaterEntryType::Reading:
      return "reading";
    case WaterEntryType::Dose:
      return "dose";
    case WaterEntryType::Cartridge:
      return "cartridge";
    case WaterEntryType::Dial:
      return "dial";
  }
  return "reading";
}

WaterEntryType entryTypeFromName(const char *name) {
  const WaterEntryType types[] = {WaterEntryType::Reading, WaterEntryType::Dose, WaterEntryType::Cartridge,
                                  WaterEntryType::Dial};
  for (WaterEntryType type : types) {
    if (strcmp(name, entryTypeName(type)) == 0) {
      return type;
    }
  }
  return WaterEntryType::Reading;
}

void putIfSet(JsonObject out, const char *key, float value) {
  if (!isnan(value)) {
    out[key] = value;
  }
}

}  // namespace

const char *sanitizerSystemName(SanitizerSystem system) {
  switch (system) {
    case SanitizerSystem::Frog:
      return "frog";
    case SanitizerSystem::Chlorine:
      return "chlorine";
    case SanitizerSystem::Bromine:
      return "bromine";
  }
  return "frog";
}

const char *cartridgeSlotName(CartridgeSlot slot) {
  return slot == CartridgeSlot::Mineral ? "mineral" : "sanitizer";
}

bool cartridgeSlotFromName(const char *name, CartridgeSlot *slot) {
  if (strcmp(name, "sanitizer") == 0) {
    *slot = CartridgeSlot::Sanitizer;
    return true;
  }
  if (strcmp(name, "mineral") == 0) {
    *slot = CartridgeSlot::Mineral;
    return true;
  }
  return false;
}

WaterLog::WaterLog() {
  slotRef(CartridgeSlot::Sanitizer).lifeDays = DefaultSanitizerLifeDays;
  slotRef(CartridgeSlot::Mineral).lifeDays = DefaultMineralLifeDays;
}

void WaterLog::append(WaterEntry entry) {
  entry.id = nextId_++;
  if (nextId_ == 0) {
    nextId_ = 1;
  }
  entries_.push_back(entry);
  if (entries_.size() > MaxEntries) {
    entries_.erase(entries_.begin());
  }
  dirty_ = true;
}

const char *WaterLog::setSystem(const std::string &name) {
  const SanitizerSystem systems[] = {SanitizerSystem::Frog, SanitizerSystem::Chlorine, SanitizerSystem::Bromine};
  for (SanitizerSystem system : systems) {
    if (name == sanitizerSystemName(system)) {
      system_ = system;
      dirty_ = true;
      return nullptr;
    }
  }
  return "unknown sanitizer system";
}

const char *WaterLog::addReading(uint32_t ts, float sanitizer, float ph, float alkalinity, float hardness,
                                 const std::string &rawNote) {
  if (isnan(sanitizer) && isnan(ph) && isnan(alkalinity) && isnan(hardness)) {
    return "enter at least one reading";
  }
  if (outside(sanitizer, 0, 20)) {
    return "sanitizer must be 0 to 20 ppm";
  }
  if (outside(ph, 6, 9)) {
    return "pH must be 6.0 to 9.0";
  }
  if (outside(alkalinity, 0, 400)) {
    return "alkalinity must be 0 to 400 ppm";
  }
  if (outside(hardness, 0, 1000)) {
    return "hardness must be 0 to 1000 ppm";
  }
  const std::string note = trimmed(rawNote);
  if (note.size() > MaxNoteLength) {
    return "note is too long (60 characters max)";
  }
  WaterEntry entry;
  entry.ts = ts;
  entry.type = WaterEntryType::Reading;
  entry.sanitizer = rounded(sanitizer);
  entry.ph = rounded(ph);
  entry.alkalinity = rounded(alkalinity);
  entry.hardness = rounded(hardness);
  entry.note = note;
  append(entry);
  return nullptr;
}

const char *WaterLog::addDose(uint32_t ts, const std::string &rawChemical, float amount, const std::string &rawUnit,
                              const std::string &rawNote) {
  const std::string chemical = trimmed(rawChemical);
  const std::string unit = trimmed(rawUnit);
  const std::string note = trimmed(rawNote);
  if (chemical.empty()) {
    return "pick a chemical";
  }
  if (chemical.size() > MaxChemicalLength) {
    return "chemical name is too long (24 characters max)";
  }
  if (isnan(amount) || amount <= 0 || amount > 1000) {
    return "amount must be more than 0";
  }
  if (unit.empty() || unit.size() > MaxUnitLength) {
    return "pick a unit";
  }
  if (note.size() > MaxNoteLength) {
    return "note is too long (60 characters max)";
  }
  WaterEntry entry;
  entry.ts = ts;
  entry.type = WaterEntryType::Dose;
  entry.chemical = chemical;
  entry.amount = rounded(amount);
  entry.unit = unit;
  entry.note = note;
  append(entry);
  return nullptr;
}

const char *WaterLog::replaceCartridge(CartridgeSlot slot, uint32_t ts, int dial) {
  if (dial > 6) {
    return "dial must be 0 to 6";
  }
  Cartridge &cartridge = slotRef(slot);
  cartridge.installedAt = ts;
  cartridge.pumpSec = 0;
  WaterEntry entry;
  entry.ts = ts;
  entry.type = WaterEntryType::Cartridge;
  entry.slot = slot;
  if (slot == CartridgeSlot::Sanitizer) {
    if (dial >= 0) {
      cartridge.dial = static_cast<uint8_t>(dial);
    }
    entry.dial = cartridge.dial;
  }
  append(entry);
  return nullptr;
}

const char *WaterLog::setDial(uint32_t ts, int dial) {
  if (dial < 0 || dial > 6) {
    return "dial must be 0 to 6";
  }
  slotRef(CartridgeSlot::Sanitizer).dial = static_cast<uint8_t>(dial);
  WaterEntry entry;
  entry.ts = ts;
  entry.type = WaterEntryType::Dial;
  entry.dial = static_cast<uint8_t>(dial);
  append(entry);
  return nullptr;
}

const char *WaterLog::setLifeDays(CartridgeSlot slot, int days) {
  if (days < 1 || days > MaxLifeDays) {
    return "replace every 1 to 365 days";
  }
  slotRef(slot).lifeDays = static_cast<uint16_t>(days);
  dirty_ = true;
  return nullptr;
}

const char *WaterLog::deleteEntry(uint16_t id) {
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].id == id) {
      entries_.erase(entries_.begin() + static_cast<long>(i));
      dirty_ = true;
      return nullptr;
    }
  }
  return "entry not found";
}

void WaterLog::addPumpTime(uint32_t seconds) {
  if (system_ != SanitizerSystem::Frog) {
    return;  // cartridges out of use keep their totals for when FROG comes back
  }
  for (Cartridge &cartridge : cartridges_) {
    if (cartridge.installed()) {
      cartridge.pumpSec += seconds;
      dirty_ = true;
    }
  }
}

void WaterLog::toJson(JsonObject out) const {
  out["system"] = sanitizerSystemName(system_);
  out["nextId"] = nextId_;
  JsonObject cartridges = out["cartridges"].to<JsonObject>();
  for (CartridgeSlot slot : {CartridgeSlot::Sanitizer, CartridgeSlot::Mineral}) {
    const Cartridge &cartridge = this->cartridge(slot);
    JsonObject item = cartridges[cartridgeSlotName(slot)].to<JsonObject>();
    item["installedAt"] = cartridge.installedAt;
    item["pumpSec"] = cartridge.pumpSec;
    item["lifeDays"] = cartridge.lifeDays;
    if (slot == CartridgeSlot::Sanitizer && cartridge.dial != DialUnknown) {
      item["dial"] = cartridge.dial;
    }
  }
  JsonArray entries = out["entries"].to<JsonArray>();
  for (const WaterEntry &entry : entries_) {
    JsonObject item = entries.add<JsonObject>();
    item["id"] = entry.id;
    item["ts"] = entry.ts;
    item["type"] = entryTypeName(entry.type);
    switch (entry.type) {
      case WaterEntryType::Reading:
        putIfSet(item, "san", entry.sanitizer);
        putIfSet(item, "ph", entry.ph);
        putIfSet(item, "ta", entry.alkalinity);
        putIfSet(item, "th", entry.hardness);
        break;
      case WaterEntryType::Dose:
        item["chem"] = entry.chemical;
        item["amount"] = entry.amount;
        item["unit"] = entry.unit;
        break;
      case WaterEntryType::Cartridge:
        item["slot"] = cartridgeSlotName(entry.slot);
        if (entry.dial != DialUnknown) {
          item["dial"] = entry.dial;
        }
        break;
      case WaterEntryType::Dial:
        item["dial"] = entry.dial;
        break;
    }
    if (!entry.note.empty()) {
      item["note"] = entry.note;
    }
  }
}

void WaterLog::fromJson(JsonVariantConst in) {
  *this = WaterLog();
  setSystem(in["system"] | "frog");
  nextId_ = in["nextId"] | 1;
  for (CartridgeSlot slot : {CartridgeSlot::Sanitizer, CartridgeSlot::Mineral}) {
    JsonObjectConst item = in["cartridges"][cartridgeSlotName(slot)];
    Cartridge &cartridge = slotRef(slot);
    cartridge.installedAt = item["installedAt"] | 0;
    cartridge.pumpSec = item["pumpSec"] | 0;
    cartridge.lifeDays = item["lifeDays"] | cartridge.lifeDays;
    cartridge.dial = item["dial"] | DialUnknown;
  }
  for (JsonObjectConst item : in["entries"].as<JsonArrayConst>()) {
    WaterEntry entry;
    entry.id = item["id"] | 0;
    entry.ts = item["ts"] | 0;
    entry.type = entryTypeFromName(item["type"] | "reading");
    entry.sanitizer = item["san"] | NAN;
    entry.ph = item["ph"] | NAN;
    entry.alkalinity = item["ta"] | NAN;
    entry.hardness = item["th"] | NAN;
    entry.chemical = item["chem"] | "";
    entry.amount = item["amount"] | NAN;
    entry.unit = item["unit"] | "";
    cartridgeSlotFromName(item["slot"] | "sanitizer", &entry.slot);
    entry.dial = item["dial"] | DialUnknown;
    entry.note = item["note"] | "";
    if (entry.id == 0) {
      continue;
    }
    if (entry.id >= nextId_) {
      nextId_ = entry.id + 1;
    }
    entries_.push_back(entry);
    if (entries_.size() > MaxEntries) {
      entries_.erase(entries_.begin());
    }
  }
  dirty_ = false;
}
