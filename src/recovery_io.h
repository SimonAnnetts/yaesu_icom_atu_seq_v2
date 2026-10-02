#pragma once

#include <Arduino.h>

// EEPROM side of crash recovery (see recovery.h).

// Record "tune in progress, original mode X". Call just before the radio is changed.
void recoveryArm(uint8_t originalMode);

// Clear the record: the radio is unkeyed and back in its own mode.
void recoveryDisarm();

// At boot, after the CAT port is up: if the last run ended mid-tune, unkey the radio,
// restore its mode and clear the record. Blocks for about a quarter of a second, and
// only when there is something to recover.
void recoveryRunIfNeeded();
