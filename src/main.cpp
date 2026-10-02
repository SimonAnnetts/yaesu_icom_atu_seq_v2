#include <Arduino.h>

#include "ah4_io.h"
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

// Wiring only: pin setup, the ALC charge pump PWM, and the main loop that
// drives the sequencer, the CAT bridge and the bench walk-test. The activity LED
// lights while CAT bytes are flowing.
// The ALC charge pump PWM (D9) is started at ~15kHz, with its opto gate (D10)
// left off so nothing reaches the radio's ALC line yet.

constexpr uint32_t LED_HOLD_MS = 20;

// ALC charge pump drive: D9 is OC2B (Timer2). Fast PWM with OCR2A as TOP and
// a /8 prescaler gives 16MHz / (8 * (133 + 1)) = ~14.9kHz. (Timer2's plain
// 8-bit modes can't hit 15kHz: /1 is 31.4kHz or more, /8 is 3.9kHz or less.)
// OCR2A is used as TOP, so D10 loses its PWM function - fine, it's a plain
// on/off gate. Duty sets the pump's output; 50% is a starting point to tune
// against the real circuit (measure the -V rail with the gate off).
constexpr uint8_t ALC_PWM_TOP = 133;                     // f = 16MHz / (8 * (TOP + 1))
constexpr uint8_t ALC_PWM_DUTY = (ALC_PWM_TOP + 1) / 2;  // OCR2B, 0..TOP+1 = 0..100%
static_assert(PIN_ALC_PWM == 9, "ALC PWM setup below assumes Timer2 / OC2B on D9");

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

static void startAlcPump() {
  OCR2A = ALC_PWM_TOP;
  OCR2B = ALC_PWM_DUTY;
  TCCR2A = _BV(COM2B1) | _BV(WGM21) | _BV(WGM20); // fast PWM (mode 7), OC2B non-inverting
  TCCR2B = _BV(WGM22) | _BV(CS21);                // TOP = OCR2A, prescaler /8
}

void setup() {
  configurePins();
  walktestBegin([] {
    configurePins();
    sequencerIoInvalidate();
    ah4IoResync();
  });

  Serial.begin(115200);
  watchdogReportBoot();
  catBridgeBegin();
  recoveryRunIfNeeded(); // undo a tune that a reset interrupted, before anything else uses CAT

  startAlcPump();
  configIoBegin();
  sequencerIoBegin();
  ah4IoBegin();
  buttonIoBegin();
  tuneIoBegin();

  Serial.println(F("ALC pump PWM on D9: ~14.9kHz, gate (D10) off"));
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
  }
  watchdogStage(WDT_STAGE_CONFIG);
  configIoPoll();
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
