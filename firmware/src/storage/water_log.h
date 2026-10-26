#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include <ArduinoJson.h>

enum class SanitizerSystem : uint8_t {
  Frog,  // FROG @ease: SmartChlor + mineral cartridges, low chlorine
  Chlorine,
  Bromine,
};

enum class CartridgeSlot : uint8_t {
  Sanitizer,  // FROG SmartChlor
  Mineral,
};

// SmartChlor dial position not recorded. 0 is a real setting: closed, fine for a lightly used tub.
constexpr uint8_t DialUnknown = 0xFF;

// A FROG cartridge in the tub. installedAt == 0 means none has been recorded.
struct Cartridge {
  uint32_t installedAt = 0;
  uint32_t pumpSec = 0;    // water moving past it since it went in
  uint16_t lifeDays = 0;   // replace after this many days
  uint8_t dial = DialUnknown;  // SmartChlor setting 0 (closed) to 6 (unused for the mineral cartridge)

  bool installed() const { return installedAt != 0; }
};

enum class WaterEntryType : uint8_t {
  Reading,    // a test strip
  Dose,       // a chemical added by hand
  Cartridge,  // a FROG cartridge replaced
  Dial,       // the SmartChlor dial turned
};

struct WaterEntry {
  uint16_t id = 0;
  uint32_t ts = 0;
  WaterEntryType type = WaterEntryType::Reading;
  // Reading: NaN for a pad that wasn't read.
  float sanitizer = NAN;  // free chlorine or bromine, ppm
  float ph = NAN;
  float alkalinity = NAN;  // ppm
  float hardness = NAN;    // ppm
  // Dose
  std::string chemical;
  float amount = NAN;
  std::string unit;
  // Cartridge / Dial
  CartridgeSlot slot = CartridgeSlot::Sanitizer;
  uint8_t dial = DialUnknown;
  std::string note;
};

// Water chemistry: test strip readings, chemicals added and FROG cartridges. Times are Unix seconds.
// Methods return nullptr on success or an error message for the user.
class WaterLog {
 public:
  static constexpr size_t MaxEntries = 150;
  static constexpr size_t MaxChemicalLength = 24;
  static constexpr size_t MaxUnitLength = 8;
  static constexpr size_t MaxNoteLength = 60;
  static constexpr uint16_t DefaultSanitizerLifeDays = 28;  // FROG: 3-5 weeks depending on the dial
  static constexpr uint16_t DefaultMineralLifeDays = 120;   // FROG: 4 months
  static constexpr uint16_t MaxLifeDays = 365;

  WaterLog();

  const char *setSystem(const std::string &name);
  const char *addReading(uint32_t ts, float sanitizer, float ph, float alkalinity, float hardness,
                         const std::string &note);
  const char *addDose(uint32_t ts, const std::string &chemical, float amount, const std::string &unit,
                      const std::string &note);
  // dial (0-6) is only kept for the SmartChlor; a negative dial leaves it unchanged.
  const char *replaceCartridge(CartridgeSlot slot, uint32_t ts, int dial);
  const char *setDial(uint32_t ts, int dial);
  const char *setLifeDays(CartridgeSlot slot, int days);
  // Removes a mistaken entry. Cartridge state is not rolled back.
  const char *deleteEntry(uint16_t id);

  // Credits pump run time to the FROG cartridges in the tub.
  void addPumpTime(uint32_t seconds);

  void toJson(JsonObject out) const;
  void fromJson(JsonVariantConst in);

  SanitizerSystem system() const { return system_; }
  const Cartridge &cartridge(CartridgeSlot slot) const { return cartridges_[static_cast<size_t>(slot)]; }
  const std::vector<WaterEntry> &entries() const { return entries_; }
  bool dirty() const { return dirty_; }
  void clearDirty() { dirty_ = false; }

 private:
  Cartridge &slotRef(CartridgeSlot slot) { return cartridges_[static_cast<size_t>(slot)]; }
  void append(WaterEntry entry);

  SanitizerSystem system_ = SanitizerSystem::Frog;
  Cartridge cartridges_[2];
  std::vector<WaterEntry> entries_;
  uint16_t nextId_ = 1;
  bool dirty_ = false;
};

const char *sanitizerSystemName(SanitizerSystem system);
const char *cartridgeSlotName(CartridgeSlot slot);
bool cartridgeSlotFromName(const char *name, CartridgeSlot *slot);
