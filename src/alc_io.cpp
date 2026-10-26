#include "alc_io.h"

#include "pins.h"
#include "sequencer_io.h"

// Timer2 fast PWM with OCR2A as TOP and a /8 prescaler: 16MHz / (8 * (133 + 1)) =
// ~14.9kHz. (Timer2's plain 8-bit modes can't hit 15kHz: /1 is 31.4kHz or more, /8
// is 3.9kHz or less.)
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

static void printHex8(uint8_t v) {
  Serial.print(F("0x"));
  if (v < 0x10) Serial.print('0');
  Serial.print(v, HEX);
}

void alcPumpSelfTest() {
  if (!sequencerIoAllIdle()) {
    Serial.println(F("ALC: self-test refused, a band is sequencing"));
    return;
  }
  Serial.print(F("ALC pump registers: TCCR2A="));
  printHex8(TCCR2A);
  Serial.print(F(" TCCR2B="));
  printHex8(TCCR2B);
  Serial.print(F(" OCR2A="));
  Serial.print(OCR2A);
  Serial.print(F(" OCR2B="));
  Serial.println(OCR2B);
  Serial.println(F("  expect: TCCR2A=0x23 TCCR2B=0x0A OCR2A=133 OCR2B=67 -> 14.93kHz, 50.7% high"));

  volatile uint8_t *pin = portInputRegister(digitalPinToPort(PIN_ALC_PWM));
  const uint8_t mask = digitalPinToBitMask(PIN_ALC_PWM);
  constexpr uint16_t SAMPLES = 12000;

  // Timer1 as a 0.5us stopwatch for the duration of the test (nothing else uses it).
  const uint8_t a = TCCR1A, b = TCCR1B;
  const uint8_t sreg = SREG;
  cli();
  TCCR1A = 0;
  TCCR1B = _BV(CS11); // normal mode, /8
  uint16_t high = 0, edges = 0;
  uint8_t prev = *pin & mask;
  TCNT1 = 0;
  for (uint16_t i = 0; i < SAMPLES; i++) {
    uint8_t now = *pin & mask;
    if (now) high++;
    if (now && !prev) edges++;
    prev = now;
  }
  uint16_t ticks = TCNT1;
  TCCR1A = a;
  TCCR1B = b;
  SREG = sreg;

  // duty in 0.1% steps; frequency in Hz from edges over the measured time (0.5us per tick)
  uint32_t dutyTenths = (uint32_t)high * 1000UL / SAMPLES;
  uint32_t hz = edges ? (uint32_t)edges * 2000000UL / ticks : 0;
  Serial.print(F("  measured on D9: "));
  Serial.print(dutyTenths / 10);
  Serial.print('.');
  Serial.print(dutyTenths % 10);
  Serial.print(F("% high, "));
  Serial.print(hz / 1000);
  Serial.print('.');
  Serial.print(hz % 1000 / 10);
  Serial.print(F("kHz ("));
  Serial.print(edges);
  Serial.print(F(" edges in "));
  Serial.print(ticks / 2);
  Serial.println(F("us)"));
  if (edges == 0) Serial.println(F("  no edges: D9 is not toggling (PWM disconnected or stuck)"));
}

bool alcHandleChar(char c) {
  switch (c) {
    case 'w':
      alcPumpSelfTest();
      return true;
    case 'g':
      setGate(!gate, false);
      Serial.println(gate ? F("ALC: gate ON (the pump voltage is on the radio's ALC line; auto-off in 30s)")
                          : F("ALC: gate off"));
      return true;
    case '?':
      Serial.println(F("alc keys: g gate on/off, w pump self-test"));
      return false; // other handlers print their help too
  }
  return false;
}
