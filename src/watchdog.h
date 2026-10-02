#pragma once

#include <Arduino.h>

// Hardware watchdog: 2s, in interrupt-then-reset mode. The main loop feeds it every
// pass; if the loop stalls, the first expiry runs an interrupt that leaves a note in
// EEPROM (with the loop stage that was running), and the second expiry resets the
// board. The note is how the next boot knows why it was reset: the Arduino
// bootloader clears the reset-cause register, so MCUSR can't say.
//
// A reset puts every pin back to an input, so optos and LEDs go off and TX INHIBIT
// is released; a tune in progress is undone by recovery.h at the next boot.

enum WdtStage : uint8_t {
  WDT_STAGE_SETUP = 1,
  WDT_STAGE_SERIAL = 2,
  WDT_STAGE_CONFIG = 3,
  WDT_STAGE_SEQUENCER = 4,
  WDT_STAGE_AH4 = 5,
  WDT_STAGE_TUNE = 6,
  WDT_STAGE_CAT = 7,
  WDT_STAGE_RADIO = 8,
};

// Print what the previous run left behind (a watchdog note), and clear it.
void watchdogReportBoot();

// Start the watchdog. Call last in setup().
void watchdogBegin();

// Call every loop(), and inside anything that can block for long (EEPROM writes).
void watchdogFeed();

// Record which part of the loop is running, for the stall note. Cheap.
void watchdogStage(uint8_t stage);

// Serial0 key '!': stall on purpose to prove the watchdog resets the board.
bool watchdogHandleChar(char c);
