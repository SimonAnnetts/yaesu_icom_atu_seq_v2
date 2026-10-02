#include "recovery_io.h"

#include <EEPROM.h>

#include "config.h"
#include "eeprom_map.h"
#include "recovery.h"

static_assert(EEPROM_CONFIG_ADDR + CONFIG_IMAGE_MAX <= EEPROM_RECOVERY_ADDR, "EEPROM regions overlap");

void recoveryArm(uint8_t originalMode) {
  uint8_t rec[RECOVERY_RECORD_LEN];
  recoveryEncode(originalMode, rec);
  // update() only writes bytes that differ; the magic goes last so a power loss
  // part-way never leaves a record that looks complete but is wrong
  for (int i = RECOVERY_RECORD_LEN - 1; i >= 0; i--) EEPROM.update(EEPROM_RECOVERY_ADDR + i, rec[i]);
}

void recoveryDisarm() {
  for (int i = 0; i < RECOVERY_RECORD_LEN; i++) EEPROM.update(EEPROM_RECOVERY_ADDR + i, 0xFF);
}

void recoveryRunIfNeeded() {
  uint8_t rec[RECOVERY_RECORD_LEN];
  for (int i = 0; i < RECOVERY_RECORD_LEN; i++) rec[i] = EEPROM.read(EEPROM_RECOVERY_ADDR + i);
  uint8_t mode;
  if (!recoveryDecode(rec, mode)) return;

  Serial.print(F("RECOVERY: the last tune did not finish. Unkeying the radio, restoring mode 0x"));
  Serial.println(mode, HEX);

  RecoveryStep steps[4];
  uint8_t n = recoveryScript(mode, steps);
  while (Serial3.available()) Serial3.read();
  for (uint8_t i = 0; i < n; i++) {
    Serial3.write(steps[i].cmd, 5);
    delay(steps[i].delayAfterMs);
  }
  while (Serial3.available()) Serial3.read(); // nobody is waiting for these replies
  recoveryDisarm();
  Serial.println(F("RECOVERY: done"));
}
