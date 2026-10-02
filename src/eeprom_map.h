#pragma once

// Where things live in the Mega's 4KB EEPROM. One place, so two features can't
// quietly claim the same bytes.

constexpr int EEPROM_CONFIG_ADDR = 0;     // sequencer config image, up to CONFIG_IMAGE_MAX (160) bytes
constexpr int EEPROM_RECOVERY_ADDR = 200; // 3 bytes: crash-recovery record (recovery.h)
constexpr int EEPROM_WDT_ADDR = 210;      // 2 bytes: "watchdog fired" note + the loop stage it was in
