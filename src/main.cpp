#include <Arduino.h>

#include "pins.h"
#include "sequencer_io.h"
#include "walktest.h"

// Basic CAT passthrough: Serial2 (Port 1, PC) <-> Serial3 (Port 2, radio),
// byte for byte in both directions. The activity LED lights while bytes are
// flowing. Every byte forwarded is also logged to Serial0 as hex, 5 bytes per
// line (one FT-847 CAT frame), tagged with its direction. A partial line is
// flushed after a short idle gap (e.g. the radio's 1-byte status replies).
// The ALC charge pump PWM (D9) is also started, at ~15kHz, with its opto gate
// (D10) left off so nothing reaches the radio's ALC line yet.

constexpr uint32_t CAT_BAUD = 57600;
constexpr uint8_t CAT_CONFIG = SERIAL_8N2; // FT-847: 8 data bits, 2 stop, no parity
constexpr uint32_t LED_HOLD_MS = 20;

constexpr uint8_t BYTES_PER_LINE = 5;
constexpr uint32_t FLUSH_GAP_MS = 10;
constexpr int LINE_MAX_CHARS = 32; // skip logging rather than block forwarding

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
  initOutput(PIN_TX_INHIBIT, LOW); // released
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

struct HexLog {
  const char *label;
  uint8_t buf[BYTES_PER_LINE];
  uint8_t count;
  uint32_t lastByteAt;

  void add(uint8_t b) {
    buf[count++] = b;
    lastByteAt = millis();
    if (count == BYTES_PER_LINE) flush();
  }

  void flush() {
    if (count == 0) return;
    // Forwarding matters more than logging: if Serial0's TX buffer is full,
    // drop this line instead of blocking.
    if (Serial.availableForWrite() >= LINE_MAX_CHARS) {
      Serial.print(label);
      for (uint8_t i = 0; i < count; i++) {
        Serial.print(' ');
        if (buf[i] < 0x10) Serial.print('0');
        Serial.print(buf[i], HEX);
      }
      Serial.println();
    }
    count = 0;
  }

  void flushIfIdle() {
    if (count && (uint32_t)(millis() - lastByteAt) >= FLUSH_GAP_MS) flush();
  }
};

HexLog pcToRadio = {"PC>RADIO", {}, 0, 0};
HexLog radioToPc = {"RADIO>PC", {}, 0, 0};

// Move every waiting byte from one port to the other; true if any moved.
static bool forward(Stream &from, Stream &to, HexLog &log) {
  bool moved = false;
  while (from.available()) {
    uint8_t b = from.read();
    to.write(b);
    log.add(b);
    moved = true;
  }
  return moved;
}

void setup() {
  configurePins();
  walktestBegin([] {
    configurePins();
    sequencerIoInvalidate();
  });

  Serial.begin(115200);
  Serial2.begin(CAT_BAUD, CAT_CONFIG);
  Serial3.begin(CAT_BAUD, CAT_CONFIG);

  startAlcPump();
  sequencerIoBegin();

  Serial.println(F("ALC pump PWM on D9: ~14.9kHz, gate (D10) off"));
  Serial.println(F("CAT passthrough: Serial2 (PC) <-> Serial3 (radio), 57600 8N2"));
}

void loop() {
  walktestPoll();
  if (walktestActive()) return; // bench walk-test owns the pins and Serial0

  sequencerIoPoll();

  bool moved = forward(Serial2, Serial3, pcToRadio);
  moved |= forward(Serial3, Serial2, radioToPc);
  pcToRadio.flushIfIdle();
  radioToPc.flushIfIdle();

  if (moved) {
    digitalWrite(PIN_ACTIVITY_LED, HIGH);
    ledOffAt = millis() + LED_HOLD_MS;
  } else if (digitalRead(PIN_ACTIVITY_LED) && (int32_t)(millis() - ledOffAt) >= 0) {
    digitalWrite(PIN_ACTIVITY_LED, LOW);
  }
}
