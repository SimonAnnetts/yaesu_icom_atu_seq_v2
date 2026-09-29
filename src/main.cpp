#include <Arduino.h>

#include "pins.h"

// Pin setup only, for now - proves pins.h against the real hardware
// before any CAT/ATU/sequencer logic is built on top of it.

void setup() {
  pinMode(PIN_TUNE_BUTTON, INPUT_PULLUP);
  pinMode(PIN_TUNE_LED, OUTPUT);
  pinMode(PIN_ACTIVITY_LED, OUTPUT);

  pinMode(PIN_ALC_PWM, OUTPUT);
  pinMode(PIN_ALC_GATE, OUTPUT);

  pinMode(PIN_AH4_KEY, INPUT_PULLUP);
  pinMode(PIN_AH4_START, OUTPUT);

  pinMode(PIN_TX_INHIBIT, OUTPUT);
  digitalWrite(PIN_TX_INHIBIT, LOW); // idle = not inhibited (active-high)
  pinMode(PIN_TX_GND, INPUT_PULLUP);

  for (uint8_t band = 0; band < BAND_COUNT; band++) {
    pinMode(PIN_STBY[band], INPUT_PULLUP);

    pinMode(PIN_SEQ[band].rx, OUTPUT);
    pinMode(PIN_SEQ[band].seq1, OUTPUT);
    pinMode(PIN_SEQ[band].seq2, OUTPUT);
    pinMode(PIN_SEQ[band].seq3, OUTPUT);
    pinMode(PIN_SEQ[band].tx, OUTPUT);

    digitalWrite(PIN_SEQ[band].rx, HIGH); // idle: RX lit, everything else off
    digitalWrite(PIN_SEQ[band].seq1, LOW);
    digitalWrite(PIN_SEQ[band].seq2, LOW);
    digitalWrite(PIN_SEQ[band].seq3, LOW);
    digitalWrite(PIN_SEQ[band].tx, LOW);
  }
}

void loop() {}
