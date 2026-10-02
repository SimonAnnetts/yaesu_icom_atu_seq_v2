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

bool configIoHandleChar(char c) { return receiver.handleChar(c, millis()); }

void configIoPoll() { receiver.poll(millis()); }
