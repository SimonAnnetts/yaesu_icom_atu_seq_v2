#include "sequencer_io.h"

#include <Arduino.h>

#include "pins.h"
#include "sequencer.h"

static_assert(BAND_COUNT == SEQ_BANDS, "pins.h Band and sequencer band counts differ");

// Assertion (PTT pressed) is acted on at once; release must stay high this long
// before the down-sequence starts, so a contact bounce can't drop the stages.
constexpr uint32_t STBY_RELEASE_DEBOUNCE_MS = 5;
constexpr int LOG_LINE_MAX = 60;

static SequencerConfig activeConfig = DEFAULT_SEQUENCER_CONFIG;
static Sequencer sequencer(activeConfig);

// ISRs only note that a falling edge (STBY assert) happened; the loop acts on
// it. That way an assert pulse shorter than one loop pass is still seen.
static volatile uint8_t pendingAssert = 0;
static void isrHf() { pendingAssert |= _BV(BAND_HF); }
static void isr50m() { pendingAssert |= _BV(BAND_50M); }
static void isr144m() { pendingAssert |= _BV(BAND_144M); }
static void isr430m() { pendingAssert |= _BV(BAND_430M); }

static bool stbyActive[BAND_COUNT];
static bool releasing[BAND_COUNT];
static uint32_t releaseSince[BAND_COUNT];

static SequencerOutputs lastOut;
static bool lastValid = false;

static const char *const BAND_NAMES[BAND_COUNT] = {"HF", "50M", "144M", "430M"};

void sequencerIoBegin() {
  attachInterrupt(digitalPinToInterrupt(PIN_STBY[BAND_HF]), isrHf, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_STBY[BAND_50M]), isr50m, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_STBY[BAND_144M]), isr144m, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_STBY[BAND_430M]), isr430m, FALLING);
}

void sequencerIoInvalidate() { lastValid = false; }

const SequencerConfig &sequencerIoConfig() { return activeConfig; }

bool sequencerIoApplyConfig(const SequencerConfig &cfg) {
  if (!sequencer.allIdle()) return false;
  activeConfig = cfg;
  return true;
}

void sequencerIoSetHold(uint8_t band, bool held) { sequencer.setHold(band, held); }
void sequencerIoRequest(uint8_t band, bool wantTx) {
  sequencer.request(band, wantTx, Profile::Tune, millis());
}
bool sequencerIoActive(uint8_t band) { return sequencer.active(band); }
bool sequencerIoIdle(uint8_t band) { return sequencer.idle(band); }
bool sequencerIoAllIdle() { return sequencer.allIdle(); }

static void pollStby(uint32_t now) {
  noInterrupts();
  uint8_t pending = pendingAssert;
  pendingAssert = 0;
  interrupts();

  for (uint8_t b = 0; b < BAND_COUNT; b++) {
    bool asserted = digitalRead(PIN_STBY[b]) == LOW || (pending & _BV(b));
    if (asserted) {
      releasing[b] = false;
      if (!stbyActive[b]) {
        stbyActive[b] = true;
        sequencer.stby(b, true, now);
      }
    } else if (stbyActive[b]) {
      if (!releasing[b]) {
        releasing[b] = true;
        releaseSince[b] = now;
      } else if ((uint32_t)(now - releaseSince[b]) >= STBY_RELEASE_DEBOUNCE_MS) {
        stbyActive[b] = false;
        releasing[b] = false;
        sequencer.stby(b, false, now);
      }
    }
  }
}

static void logOutputs(uint32_t now, const SequencerOutputs &o) {
  if (Serial.availableForWrite() < LOG_LINE_MAX) return; // never block the loop
  Serial.print('[');
  Serial.print(now);
  Serial.print(F("] INH="));
  Serial.print(o.txInhibit);
  for (uint8_t b = 0; b < BAND_COUNT; b++) {
    Serial.print(' ');
    Serial.print(BAND_NAMES[b]);
    Serial.print('=');
    Serial.print(o.rx[b]);
    for (uint8_t s = 0; s < SEQ_STAGES; s++) Serial.print(o.seq[b][s]);
    Serial.print(o.tx[b]);
  }
  Serial.println(); // per band: RX SEQ1 SEQ2 SEQ3 TX
}

static void writeOutputs(const SequencerOutputs &o) {
  digitalWrite(PIN_TX_INHIBIT, o.txInhibit);
  for (uint8_t b = 0; b < BAND_COUNT; b++) {
    const SequencerPins &p = PIN_SEQ[b];
    digitalWrite(p.rx, o.rx[b]);
    digitalWrite(p.seq1, o.seq[b][0]);
    digitalWrite(p.seq2, o.seq[b][1]);
    digitalWrite(p.seq3, o.seq[b][2]);
    digitalWrite(p.tx, o.tx[b]);
  }
}

void sequencerIoPoll() {
  uint32_t now = millis();
  pollStby(now);
  sequencer.poll(now);

  SequencerOutputs out = sequencer.outputs();
  if (lastValid && out == lastOut) return;
  writeOutputs(out);
  logOutputs(now, out);
  lastOut = out;
  lastValid = true;
}
