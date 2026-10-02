#include <Arduino.h>

#include "ah4_io.h"
#include "alc_io.h"
#include "button_io.h"
#include "cat_bridge.h"
#include "config_io.h"
#include "pins.h"
#include "radio.h"
#include "recovery_io.h"
#include "sequencer_io.h"
#include "tune_io.h"
#include "walktest.h"
#include "watchdog.h"

// Wiring only: pin setup and the main loop, which drives the sequencer, the CAT
// bridge, the tune cycle and the bench walk-test. The activity LED lights while CAT
// bytes are flowing. The ALC pump and gate live in alc_io.cpp.

constexpr uint32_t LED_HOLD_MS = 20;

uint32_t ledOffAt = 0;

// Output pin: drive the idle level first, then switch to output, so the pin
// never glitches to the wrong level.
static void initOutput(uint8_t pin, uint8_t level) {
  digitalWrite(pin, level); // on an input pin this just selects pull-up on/off
  pinMode(pin, OUTPUT);
}

// First thing in setup(): put every pin in its right mode at its idle level.
static void configurePins() {
  // Inputs
  pinMode(PIN_TUNE_BUTTON, INPUT_PULLUP);
  pinMode(PIN_AH4_KEY, INPUT_PULLUP);
  pinMode(PIN_TX_GND, INPUT_PULLUP);
  for (uint8_t band = 0; band < BAND_COUNT; band++) {
    pinMode(PIN_STBY[band], INPUT_PULLUP);
  }

  // Outputs, all idle/off
  initOutput(PIN_TUNE_LED, LOW);
  initOutput(PIN_ACTIVITY_LED, LOW);
  initOutput(PIN_ALC_PWM, LOW);
  initOutput(PIN_ALC_GATE, LOW);  // gate off: radio's ALC line not connected
  initOutput(PIN_AH4_START, LOW);
  // A STBY line already low means the radio is transmitting (we have just rebooted
  // mid-transmission): hold TX INHIBIT from the first instant, not from whenever the
  // sequencer gets going. Released otherwise.
  delayMicroseconds(100); // let the pull-ups settle
  bool stbyAsserted = false;
  for (uint8_t band = 0; band < BAND_COUNT; band++) {
    if (digitalRead(PIN_STBY[band]) == LOW) stbyAsserted = true;
  }
  initOutput(PIN_TX_INHIBIT, stbyAsserted ? HIGH : LOW);
  for (uint8_t band = 0; band < BAND_COUNT; band++) {
    const SequencerPins &p = PIN_SEQ[band];
    initOutput(p.rx, HIGH); // RX LED is lit when fully idle
    initOutput(p.seq1, LOW);
    initOutput(p.seq2, LOW);
    initOutput(p.seq3, LOW);
    initOutput(p.tx, LOW);
  }
}

void setup() {
  configurePins();
  walktestBegin([] {
    configurePins();
    alcResync(); // configurePins() drops D9's PWM output as a side effect
    sequencerIoInvalidate();
    ah4IoResync();
  });

  Serial.begin(115200);
  watchdogReportBoot();
  catBridgeBegin();
  recoveryRunIfNeeded(); // undo a tune that a reset interrupted, before anything else uses CAT

  alcBegin();
  configIoBegin();
  sequencerIoBegin();
  ah4IoBegin();
  buttonIoBegin();
  tuneIoBegin();

  Serial.println(F("CAT passthrough: Serial2 (PC) <-> Serial3 (radio), 57600 8N2; c = frame log, ? = radio keys"));
  watchdogBegin(); // last: from here the loop must keep feeding it
}

void loop() {
  watchdogFeed();
  watchdogStage(WDT_STAGE_SERIAL);
  while (Serial.available()) {
    char c = Serial.read();
    if (configIoHandleChar(c)) continue; // a config upload is in progress (it may contain any byte)
    if (watchdogHandleChar(c)) continue;
    if (!tuneIoActive() && walktestHandleChar(c)) continue; // never enter the walk-test mid-tune
    if (tuneIoHandleChar(c)) continue;
    catBridgeHandleChar(c);
    if (tuneIoActive()) continue; // the tune cycle owns the CAT bus and the AH-4
    radioHandleChar(c);
    ah4IoHandleChar(c);
    alcHandleChar(c);
  }
  watchdogStage(WDT_STAGE_CONFIG);
  configIoPoll();
  alcPoll();
  walktestPoll();
  if (walktestActive()) return; // bench walk-test owns the pins and Serial0

  watchdogStage(WDT_STAGE_SEQUENCER);
  sequencerIoPoll();
  watchdogStage(WDT_STAGE_AH4);
  ah4IoPoll();
  if (buttonIoPoll()) tuneIoButtonPress();
  watchdogStage(WDT_STAGE_TUNE);
  tuneIoPoll();

  watchdogStage(WDT_STAGE_CAT);
  bool moved = catBridgePoll();
  watchdogStage(WDT_STAGE_RADIO);
  radioPoll();

  if (moved) {
    digitalWrite(PIN_ACTIVITY_LED, HIGH);
    ledOffAt = millis() + LED_HOLD_MS;
  } else if (digitalRead(PIN_ACTIVITY_LED) && (int32_t)(millis() - ledOffAt) >= 0) {
    digitalWrite(PIN_ACTIVITY_LED, LOW);
  }
}
