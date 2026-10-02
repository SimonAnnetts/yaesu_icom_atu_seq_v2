#pragma once

#include <stdint.h>

// Recovery after a reset in the middle of a tune. A CAT PTT stays on until the radio
// is told otherwise, and the radio is left in the tune mode, so a reset (watchdog,
// brown-out, USB, power loss) mid-tune would leave it keyed and in AM. The tune cycle
// therefore records "tune in progress, original mode X" in EEPROM just before it
// changes the radio, and clears the record once the radio is safe again; at boot,
// a record that is still set means the last run did not finish, and this is the
// script that puts the radio right.
//
// (The Arduino bootloader clears the reset-cause register, so the cause of a reset
// cannot be read; this record is also a better test than the cause would be - it
// covers every kind of reset, and never unkeys a PC transmission after an
// unrelated one.)

constexpr uint8_t RECOVERY_MAGIC = 0xA7;
constexpr uint8_t RECOVERY_RECORD_LEN = 3;

// [magic][mode][~mode]: a blank, torn or corrupted record is never mistaken for one.
void recoveryEncode(uint8_t mode, uint8_t out[RECOVERY_RECORD_LEN]);
bool recoveryDecode(const uint8_t in[RECOVERY_RECORD_LEN], uint8_t &mode);

struct RecoveryStep {
  uint8_t cmd[5];
  uint16_t delayAfterMs; // the FT-847 wants ~50ms between commands
};

// What to send after an unclean end, in order: CAT on, PTT off, restore the mode (only
// if the recorded byte is a real FT-847 mode), PTT off again. Never keys the radio.
// Returns the number of steps (3 or 4).
uint8_t recoveryScript(uint8_t mode, RecoveryStep out[4]);
