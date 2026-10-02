#include "watchdog.h"

#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/wdt.h>

#include "eeprom_map.h"

constexpr uint8_t WDT_NOTE_MAGIC = 0xD7;

static volatile uint8_t stage = 0;

// The reset-cause register, saved before anything can disturb it. (With the Arduino
// bootloader it has already been cleared, so this is usually 0; it is kept in case
// the board has a different bootloader.) Also stops a watchdog left running by a
// previous reset from resetting the board again before setup() can run.
static uint8_t resetFlags __attribute__((section(".noinit")));
void earlyInit() __attribute__((naked, used, section(".init3")));
void earlyInit() {
  resetFlags = MCUSR;
  MCUSR = 0;
  wdt_disable();
}

// First expiry: leave a note, then let the second expiry reset the board. Nothing
// here may block or depend on the stalled loop.
ISR(WDT_vect) {
  eeprom_update_byte((uint8_t *)(EEPROM_WDT_ADDR + 1), stage);
  eeprom_update_byte((uint8_t *)EEPROM_WDT_ADDR, WDT_NOTE_MAGIC);
}

void watchdogStage(uint8_t s) { stage = s; }

void watchdogBegin() {
  uint8_t sreg = SREG;
  cli();
  wdt_reset();
  WDTCSR |= _BV(WDCE) | _BV(WDE);
  WDTCSR = _BV(WDIE) | _BV(WDE) | _BV(WDP2) | _BV(WDP1) | _BV(WDP0); // interrupt + reset, 2s
  SREG = sreg;
}

void watchdogFeed() {
  wdt_reset();
  // After the first expiry the hardware drops WDIE. If the loop has come back to
  // life since (a long operation, not a hang) say so and re-arm the note.
  if (!(WDTCSR & _BV(WDIE))) {
    Serial.println(F("WATCHDOG: the main loop stalled for over 2s but recovered"));
    eeprom_update_byte((uint8_t *)EEPROM_WDT_ADDR, 0xFF);
    WDTCSR |= _BV(WDIE);
  }
}

static const __FlashStringHelper *stageName(uint8_t s) {
  switch (s) {
    case WDT_STAGE_SETUP: return F("setup");
    case WDT_STAGE_SERIAL: return F("Serial0 commands");
    case WDT_STAGE_CONFIG: return F("config upload / walk-test");
    case WDT_STAGE_SEQUENCER: return F("sequencer");
    case WDT_STAGE_AH4: return F("AH-4 / tune button");
    case WDT_STAGE_TUNE: return F("tune cycle");
    case WDT_STAGE_CAT: return F("CAT bridge");
    case WDT_STAGE_RADIO: return F("radio debug");
    default: return F("unknown");
  }
}

void watchdogReportBoot() {
  if (eeprom_read_byte((const uint8_t *)EEPROM_WDT_ADDR) == WDT_NOTE_MAGIC) {
    Serial.print(F("WATCHDOG: the previous run stalled for over 2s and was reset (last in: "));
    Serial.print(stageName(eeprom_read_byte((const uint8_t *)(EEPROM_WDT_ADDR + 1))));
    Serial.println(')');
    eeprom_update_byte((uint8_t *)EEPROM_WDT_ADDR, 0xFF);
  }
  if (resetFlags) { // only non-zero if the bootloader leaves it alone
    Serial.print(F("Reset flags: 0x"));
    Serial.println(resetFlags, HEX);
  }
}

bool watchdogHandleChar(char c) {
  if (c != '!') return false;
  Serial.println(F("HANG TEST: stalling the main loop on purpose. Expect a watchdog reset in about 4s."));
  Serial.flush();
  for (;;) {
  } // never feeds the watchdog
}
