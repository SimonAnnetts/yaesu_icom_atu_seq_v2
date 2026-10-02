#include "alc_io.h"

#include "pins.h"

// Timer2 fast PWM with OCR2A as TOP and a /8 prescaler: 16MHz / (8 * (133 + 1)) =
// ~14.9kHz. (Timer2's plain 8-bit modes can't hit 15kHz: /1 is 31.4kHz or more, /8
// is 3.9kHz or less.) OCR2A is TOP, so D10 loses its PWM function - fine, it is a
// plain on/off gate.
constexpr uint8_t ALC_PWM_TOP = 133;
constexpr uint8_t DUTY_PERCENT = 50;
constexpr uint32_t BENCH_GATE_MAX_MS = 30000;
static_assert(PIN_ALC_PWM == 9, "ALC pump setup assumes Timer2 / OC2B on D9");
static_assert(PIN_ALC_GATE == 10, "the gate is a plain output on D10");

static bool gate = false;
static bool gateByTune = false;
static uint32_t gateOnAt = 0;

static void startPump() {
  pinMode(PIN_ALC_PWM, OUTPUT);
  OCR2A = ALC_PWM_TOP;
  OCR2B = (ALC_PWM_TOP + 1) * DUTY_PERCENT / 100;
  TCCR2A = _BV(COM2B1) | _BV(WGM21) | _BV(WGM20); // fast PWM (mode 7), OC2B non-inverting
  TCCR2B = _BV(WGM22) | _BV(CS21);                // TOP = OCR2A, prescaler /8
}

static void setGate(bool on, bool byTune) {
  gate = on;
  gateByTune = on && byTune;
  if (on) gateOnAt = millis();
  digitalWrite(PIN_ALC_GATE, on ? HIGH : LOW);
}

void alcBegin() {
  digitalWrite(PIN_ALC_GATE, LOW); // gate off first
  pinMode(PIN_ALC_GATE, OUTPUT);
  startPump();
  Serial.println(F("ALC pump: ~14.9kHz, 50% duty, gate off"));
}

void alcResync() {
  gate = gateByTune = false;
  digitalWrite(PIN_ALC_GATE, LOW);
  startPump();
}

void alcPoll() {
  if (gate && !gateByTune && (uint32_t)(millis() - gateOnAt) >= BENCH_GATE_MAX_MS) {
    setGate(false, false);
    Serial.println(F("ALC: gate released (left on for 30s)"));
  }
}

void alcTuneSet(bool on) { setGate(on, true); }

bool alcGate() { return gate; }

bool alcHandleChar(char c) {
  switch (c) {
    case 'g':
      setGate(!gate, false);
      Serial.println(gate ? F("ALC: gate ON (the pump voltage is on the radio's ALC line; auto-off in 30s)")
                          : F("ALC: gate off"));
      return true;
    case '?':
      Serial.println(F("alc key: g gate on/off"));
      return false; // other handlers print their help too
  }
  return false;
}
