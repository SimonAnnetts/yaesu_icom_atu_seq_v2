#pragma once

#include <Arduino.h>

// Arduino Mega 2560 pin map. Single source of truth — see README.md
// "Pin plan (Mega 2560)" for the full rationale behind each assignment.
//
// Serial0 (D0/D1): USB debug/control link to PC.
// Serial1 (D18/D19 TX/RX): unused, left free (see README) — note D18/D19
// are reused below as plain digital STBY inputs, not as Serial1.
// Serial2 (D16/D17): Port 1 - CAT to PC, via MAX202CPE.
// Serial3 (D14/D15): Port 2 - CAT to radio, via MAX202CPE.

// --- Tune control ---
constexpr uint8_t PIN_TUNE_BUTTON = 6;   // input, polled
constexpr uint8_t PIN_TUNE_LED = 7;      // output
constexpr uint8_t PIN_ACTIVITY_LED = 8;  // output, generic

// --- ALC injection (charge pump gate) ---
constexpr uint8_t PIN_ALC_PWM = 9;   // charge pump drive, PWM output
constexpr uint8_t PIN_ALC_GATE = 10; // opto-isolator on/off switch

// --- Icom AH-4 ATU interface (both via opto-isolator) ---
constexpr uint8_t PIN_AH4_KEY = 11;   // input, INPUT_PULLUP
constexpr uint8_t PIN_AH4_START = 12; // output

// --- Radio TUNER connector (8-pin mini-DIN) ---
// Pin 1 (+13.8V) and pin 2 wiring, and the deliberately-unconnected Tuner
// Sense pin, are hardware-only - no GPIO. Pin 8 (TX INHIBIT) and pin 2
// (TX_GND) are the only TUNER-connector signals this project reads/drives.
constexpr uint8_t PIN_TX_INHIBIT = 4; // output, active-high -> TUNER pin 8
constexpr uint8_t PIN_TX_GND = 5;     // input, INPUT_PULLUP <- TUNER pin 2
                                       // (open-collector, active-low; HF/50MHz
                                       // TX status only - log/cross-check, not
                                       // load-bearing)

// --- Band index, shared by STBY inputs and sequencer channels below ---
// NOTE: STBY pin order and sequencer channel order are independently
// wired and do NOT match each other - see README "Sequencer band<->channel
// mapping". Always index through Band, never assume pin-array order lines
// up between the two tables.
enum Band : uint8_t {
  BAND_HF = 0,
  BAND_50M = 1,
  BAND_144M = 2,
  BAND_430M = 3,
  BAND_COUNT = 4,
};

// --- STBY jack (5-pin mini-DIN, closure-to-ground per band) ---
// All 4 lines are on the Mega's true external-interrupt-capable pins.
constexpr uint8_t PIN_STBY[BAND_COUNT] = {
    2,  // BAND_HF
    19, // BAND_50M
    18, // BAND_144M
    3,  // BAND_430M
};

// --- Amplifier sequencer outputs, 5 per band: RX, SEQ1, SEQ2, SEQ3, TX ---
// RX/TX are LED-only indicators. SEQ1-SEQ3 each also drive a dedicated
// opto-isolator (12 opto outputs total, 3 per band).
struct SequencerPins {
  uint8_t rx;
  uint8_t seq1;
  uint8_t seq2;
  uint8_t seq3;
  uint8_t tx;
};

// D50-D53 (Band 4) are the Mega's hardware SPI pins (MISO/MOSI/SCK/SS),
// repurposed here as plain digital outputs since SPI isn't used elsewhere.
constexpr SequencerPins PIN_SEQ[BAND_COUNT] = {
    {22, 23, 24, 25, 26}, // BAND_HF
    {27, 28, 29, 30, 31}, // BAND_50M
    {44, 45, 46, 47, 48}, // BAND_144M
    {49, 50, 51, 52, 53}, // BAND_430M
};

// --- Kept free for a future I2C peripheral (e.g. a status display) ---
// D20 = SDA, D21 = SCL. Not assigned to anything; listed here only so
// nothing else accidentally claims them.
