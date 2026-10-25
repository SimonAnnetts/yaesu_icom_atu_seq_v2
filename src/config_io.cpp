#include "config_io.h"

#include <EEPROM.h>

#include "config.h"
#include "config_receiver.h"
#include "eeprom_map.h"
#include "sequencer_io.h"
#include "watchdog.h"

constexpr int EEPROM_ADDR = EEPROM_CONFIG_ADDR;

static void saveToEeprom(const SequencerConfig &cfg) {
  uint8_t buf[CONFIG_IMAGE_MAX];
  size_t n = configSerialize(cfg, buf, sizeof buf);
  for (size_t i = 0; i < n; i++) {
    EEPROM.update(EEPROM_ADDR + i, buf[i]); // writes only changed bytes, ~3.3ms each
    watchdogFeed();
  }
}

namespace {

class SerialHost : public ConfigHost {
public:
  bool idleForUpload() override { return sequencerIoAllIdle(); }
  bool apply(const SequencerConfig &cfg) override { return sequencerIoApplyConfig(cfg); }
  void save(const SequencerConfig &cfg) override { saveToEeprom(cfg); }

  void reply(Reply r, const char *detail) override {
    switch (r) {
      case Reply::Ready: Serial.println(F("READY")); break;
      case Reply::Ok: Serial.println(F("OK")); break;
      case Reply::Busy: Serial.println(F("ERROR: busy, a band is transmitting")); break;
      case Reply::ExpectedObject: Serial.println(F("ERROR: expected a JSON object")); break;
      case Reply::TooLarge: Serial.println(F("ERROR: config too large")); break;
      case Reply::TooDeep: Serial.println(F("ERROR: config nested too deeply")); break;
      case Reply::Timeout: Serial.println(F("ERROR: timeout waiting for config")); break;
      case Reply::InvalidJson:
        Serial.print(F("ERROR: invalid JSON: "));
        Serial.println(detail);
        break;
      case Reply::InvalidConfig:
        Serial.print(F("ERROR: "));
        Serial.println(detail);
        break;
    }
  }
};

SerialHost host;
ConfigReceiver receiver(host);

} // namespace

void configIoBegin() {
  uint8_t buf[CONFIG_IMAGE_MAX];
  for (size_t i = 0; i < sizeof buf; i++) buf[i] = EEPROM.read(EEPROM_ADDR + i);

  SequencerConfig cfg;
  const char *why = "";
  if (configDeserialize(buf, sizeof buf, cfg, why) && sequencerIoApplyConfig(cfg)) {
    Serial.println(F("Config: loaded from EEPROM"));
  } else {
    // Empty or invalid: stay on the built-in defaults rather than refuse to run.
    Serial.print(F("Config: using built-in defaults ("));
    Serial.print(why);
    Serial.println(')');
  }
}

static void printBand(uint8_t b, const char *name) {
  const BandConfig &c = sequencerIoConfig().band[b];
  Serial.print(name);
  Serial.print(F(": "));
  Serial.print(c.freqMinHz);
  Serial.print('-');
  Serial.print(c.freqMaxHz);
  Serial.print(F(" Hz | gaps "));
  for (uint8_t i = 0; i < SEQ_STAGES; i++) {
    Serial.print(c.gapMs[i]);
    Serial.print(i + 1 < SEQ_STAGES ? '/' : ' ');
  }
  Serial.print(F("ms | engaged when tuning: "));
  for (uint8_t i = 0; i < SEQ_STAGES; i++) {
    if (c.tuneProfile[i]) {
      Serial.print(F("SEQ"));
      Serial.print(i + 1);
    } else {
      Serial.print(F("--"));
    }
    Serial.print(' ');
  }
  Serial.print(F("(all three when transmitting) | atu "));
  Serial.print(c.atu ? F("on") : F("off"));
  Serial.print(F(" | alc "));
  Serial.println(c.alc ? F("on") : F("off"));
}

bool configIoHandleKey(char c) {
  if (c == '?') {
    Serial.println(F("config key: v show the running sequencer config"));
    return false; // other handlers print their help too
  }
  if (c != 'v') return false;
  Serial.println(F("Running sequencer config:"));
  static const char *const NAMES[SEQ_BANDS] = {"HF", "50M", "144M", "430M"};
  for (uint8_t b = 0; b < SEQ_BANDS; b++) {
    Serial.print(F("  "));
    printBand(b, NAMES[b]);
  }
  const SequencerConfig &cfg = sequencerIoConfig();
  Serial.print(F("  cross-band rules: "));
  Serial.println(cfg.triggerCount);
  for (uint8_t i = 0; i < cfg.triggerCount; i++) {
    Serial.print(F("    "));
    Serial.print(NAMES[cfg.trigger[i].sourceBand]);
    Serial.print(F(" SEQ1 -> "));
    Serial.print(NAMES[cfg.trigger[i].targetBand]);
    Serial.print(F(" SEQ"));
    Serial.println(cfg.trigger[i].targetStage + 1);
  }
  return true;
}

bool configIoHandleChar(char c) { return receiver.handleChar(c, millis()); }

void configIoPoll() { receiver.poll(millis()); }
