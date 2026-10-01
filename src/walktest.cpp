#include "walktest.h"

#include "pins.h"

constexpr uint32_t TX_INHIBIT_PULSE_MS = 200;

struct NamedPin {
  const char *name;
  uint8_t pin;
};

static const char *const BAND_NAMES[BAND_COUNT] = {"HF", "50M", "144M", "430M"};

// Outputs stepped through by n/p: 4 plain outputs + 5 per band.
constexpr uint8_t STEP_COUNT = 4 + 5 * BAND_COUNT;
static char stepNames[STEP_COUNT][14];
static uint8_t stepPins[STEP_COUNT];

static void (*restoreIdleFn)() = nullptr;
static bool active = false;
static int8_t current = -1; // index into step table, -1 = none lit
static uint32_t inhibitOffAt = 0;
static bool inhibitOn = false;

// Inputs echoed on change: D5, D6, D11 and the four STBY lines.
static NamedPin inputs[3 + BAND_COUNT];
static char inputNames[BAND_COUNT][10];
static uint8_t inputLast[3 + BAND_COUNT];

static void addStep(uint8_t &n, const char *name, uint8_t pin) {
  strncpy(stepNames[n], name, sizeof(stepNames[n]) - 1);
  stepPins[n++] = pin;
}

static void buildTables() {
  uint8_t n = 0;
  addStep(n, "TUNE_LED", PIN_TUNE_LED);
  addStep(n, "ACTIVITY_LED", PIN_ACTIVITY_LED);
  addStep(n, "ALC_GATE", PIN_ALC_GATE);
  addStep(n, "AH4_START", PIN_AH4_START);
  static const char *const roles[5] = {"RX", "SEQ1", "SEQ2", "SEQ3", "TX"};
  for (uint8_t b = 0; b < BAND_COUNT; b++) {
    const SequencerPins &p = PIN_SEQ[b];
    const uint8_t pins[5] = {p.rx, p.seq1, p.seq2, p.seq3, p.tx};
    for (uint8_t r = 0; r < 5; r++) {
      char name[14];
      snprintf(name, sizeof(name), "%s_%s", BAND_NAMES[b], roles[r]);
      addStep(n, name, pins[r]);
    }
  }

  inputs[0] = {"TX_GND", PIN_TX_GND};
  inputs[1] = {"TUNE_BTN", PIN_TUNE_BUTTON};
  inputs[2] = {"AH4_KEY", PIN_AH4_KEY};
  for (uint8_t b = 0; b < BAND_COUNT; b++) {
    snprintf(inputNames[b], sizeof(inputNames[b]), "STBY_%s", BAND_NAMES[b]);
    inputs[3 + b] = {inputNames[b], PIN_STBY[b]};
  }
}

static void allStepsOff() {
  for (uint8_t i = 0; i < STEP_COUNT; i++) digitalWrite(stepPins[i], LOW);
}

static void lightStep(int8_t idx) {
  allStepsOff();
  current = idx;
  if (idx < 0) {
    Serial.println(F("walktest: all off"));
    return;
  }
  digitalWrite(stepPins[idx], HIGH);
  Serial.print(F("walktest: "));
  Serial.print(stepNames[idx]);
  Serial.print(F(" (D"));
  Serial.print(stepPins[idx]);
  Serial.println(F(") HIGH"));
}

static void echoInputs(bool force) {
  for (uint8_t i = 0; i < 3 + BAND_COUNT; i++) {
    uint8_t v = digitalRead(inputs[i].pin);
    if (!force && v == inputLast[i]) continue;
    inputLast[i] = v;
    Serial.print(F("walktest: input "));
    Serial.print(inputs[i].name);
    Serial.print(F(" (D"));
    Serial.print(inputs[i].pin);
    Serial.print(F(") = "));
    Serial.println(v ? F("HIGH") : F("LOW"));
  }
}

static void setActive(bool on) {
  active = on;
  if (on) {
    allStepsOff();
    current = -1;
    Serial.println(F("walktest ON: t=exit n=next p=prev 0=off i=pulse TX_INH"));
    echoInputs(true);
  } else {
    inhibitOn = false;
    if (restoreIdleFn) restoreIdleFn();
    Serial.println(F("walktest OFF: outputs restored to idle"));
  }
}

void walktestBegin(void (*restoreIdle)()) {
  restoreIdleFn = restoreIdle;
  buildTables();
}

bool walktestActive() { return active; }

void walktestPoll() {
  if (inhibitOn && (int32_t)(millis() - inhibitOffAt) >= 0) {
    digitalWrite(PIN_TX_INHIBIT, LOW);
    inhibitOn = false;
    Serial.println(F("walktest: TX_INHIBIT released"));
  }

  // Serial0 is only a command port while testing; otherwise it is the log.
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 't') {
      setActive(!active);
    } else if (active) {
      switch (c) {
        case 'n': lightStep((current + 1) % STEP_COUNT); break;
        case 'p': lightStep(current <= 0 ? STEP_COUNT - 1 : current - 1); break;
        case '0': lightStep(-1); break;
        case 'i':
          digitalWrite(PIN_TX_INHIBIT, HIGH);
          inhibitOn = true;
          inhibitOffAt = millis() + TX_INHIBIT_PULSE_MS;
          Serial.println(F("walktest: TX_INHIBIT asserted (D4 HIGH)"));
          break;
      }
    }
  }

  if (active) echoInputs(false);
}
