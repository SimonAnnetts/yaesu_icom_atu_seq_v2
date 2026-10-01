#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sequencer.h"

// Pure config rules shared by the JSON loader and the EEPROM loader, so a config
// is validated the same way wherever it comes from.

constexpr uint16_t CONFIG_MAX_GAP_MS = 5000;

// Checks ranges and relationships (band edges ordered and not overlapping,
// timings bounded, triggers refer to real bands and are not self-referential).
// On failure returns false and points `err` at a static message.
bool configValidate(const SequencerConfig &cfg, const char *&err);

// EEPROM image: magic, layout version, payload length, field-by-field little-
// endian payload (not a raw struct dump, so padding can't matter), CRC-16.
constexpr size_t CONFIG_IMAGE_MAX = 160;
size_t configSerialize(const SequencerConfig &cfg, uint8_t *buf, size_t cap); // 0 if cap too small
// Rejects bad magic/version/length/CRC and anything that fails configValidate.
bool configDeserialize(const uint8_t *buf, size_t len, SequencerConfig &out, const char *&err);

uint16_t configCrc16(const uint8_t *data, size_t len); // CRC-16/CCITT-FALSE
