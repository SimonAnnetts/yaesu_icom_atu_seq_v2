#include "ah4_io.h"

#include "pins.h"

static Ah4Driver driver;
static bool fakeKey = false;
static bool lastKey = false;
static bool lastStart = false;
static uint32_t beganAt = 0;
static bool benchRun = false; // started by the 'h' key: log results

static bool keyAsserted() { return digitalRead(PIN_AH4_KEY) == LOW || fakeKey; }

static void writeStart() {
  lastStart = driver.startAsserted();
  digitalWrite(PIN_AH4_START, lastStart ? HIGH : LOW);
}

void ah4IoBegin() {
  pinMode(PIN_AH4_KEY, INPUT_PULLUP);
  digitalWrite(PIN_AH4_START, LOW);
  pinMode(PIN_AH4_START, OUTPUT);
}

void ah4IoResync() { writeStart(); }

bool ah4Begin() {
  bool k = keyAsserted();
  if (!driver.begin(k, millis())) return false;
  beganAt = millis();
  lastKey = k;
  writeStart();
  Serial.println(F("AH4: START asserted"));
  return true;
}

void ah4Abort() {
  driver.abort(millis());
  writeStart();
}

bool ah4StartAsserted() { return driver.startAsserted(); }

Ah4Driver::Result ah4TakeResult() { return driver.takeResult(); }

static const __FlashStringHelper *resultName(Ah4Driver::Result r) {
  switch (r) {
    case Ah4Driver::Result::Success: return F("success");
    case Ah4Driver::Result::NoAtu: return F("no ATU: KEY never asserted");
    case Ah4Driver::Result::Timeout: return F("timeout: KEY never released");
    case Ah4Driver::Result::TuneFailed: return F("tune failed: KEY re-asserted after releasing");
    case Ah4Driver::Result::KeyStuck: return F("refused: KEY already asserted");
    case Ah4Driver::Result::Aborted: return F("aborted");
    default: return F("?");
  }
}

void ah4IoPoll() {
  uint32_t now = millis();
  bool k = keyAsserted();
  driver.poll(k, now);
  if (driver.startAsserted() != lastStart) {
    writeStart();
    Serial.print(F("AH4: START "));
    Serial.println(lastStart ? F("asserted") : F("released"));
  }
  if (k != lastKey) {
    lastKey = k;
    Serial.print(F("AH4: KEY "));
    Serial.print(k ? F("asserted") : F("released"));
    Serial.print(F(" at +"));
    Serial.print(now - beganAt);
    Serial.println(F("ms"));
  }
  if (benchRun) {
    Ah4Driver::Result r = driver.takeResult();
    if (r != Ah4Driver::Result::None) {
      Serial.print(F("AH4: "));
      // With no RF applied a KEY cycle can't be a tune, so don't call it one.
      Serial.println(r == Ah4Driver::Result::Success
                         ? F("KEY cycle completed (no RF was applied, so not a real tune)")
                         : resultName(r));
      benchRun = false;
    }
  }
}

bool ah4IoHandleChar(char c) {
  switch (c) {
    case 'h':
      if (ah4Begin()) {
        benchRun = true;
        Serial.println(F("AH4: handshake started (no radio keyed)"));
      } else {
        Serial.println(F("AH4: busy"));
      }
      return true;
    case 'j':
      ah4Abort();
      benchRun = false;
      Serial.println(F("AH4: abort requested"));
      return true;
    case 'K':
      fakeKey = !fakeKey;
      Serial.println(fakeKey ? F("AH4: fake KEY ON") : F("AH4: fake KEY off"));
      return true;
    case '?':
      Serial.println(F("ah4 keys: h handshake (no RF), j abort, K toggle fake KEY"));
      return false; // other handlers print their help too
  }
  return false;
}
