#include "tune_io.h"

#include "ah4_io.h"
#include "alc_io.h"
#include "blinker.h"
#include "buzzer.h"
#include "cat_bridge.h"
#include "pins.h"
#include "recovery_io.h"
#include "sequencer_io.h"
#include "tune.h"

namespace {

class RealEnv : public TuneEnv {
public:
  bool catClaim(uint32_t) override { return catBridgeClaim(); }
  CatArbiter::ClaimState catClaimState() override { return catBridgeClaimState(); }
  void catRelease(uint32_t) override { catBridgeReleaseClaim(); }
  bool catSubmit(const uint8_t cmd[5], uint32_t) override { return catBridgeSubmit(cmd); }
  bool catTakeResult(CatArbiter::Result &r, uint8_t *reply, uint8_t &len) override {
    return catBridgeTakeResult(r, reply, len);
  }
  void catSetSnapshot(const CatSnapshot &s) override { catBridgeSetSnapshot(s); }
  void recoveryArm(uint8_t mode) override { ::recoveryArm(mode); }
  void recoveryDisarm() override { ::recoveryDisarm(); }
  void alcSet(bool on) override { alcTuneSet(on); }
  bool bandsAllIdle() override { return sequencerIoAllIdle(); }
  void seqHold(uint8_t band, bool held) override { sequencerIoSetHold(band, held); }
  void seqRequest(uint8_t band, bool wantTx, uint32_t) override { sequencerIoRequest(band, wantTx); }
  bool seqActive(uint8_t band) override { return sequencerIoActive(band); }
  bool seqIdle(uint8_t band) override { return sequencerIoIdle(band); }
  int8_t bandForFreq(uint32_t hz) override { return bandForFrequency(sequencerIoConfig(), hz); }
  bool atuAllowed(uint8_t band) override { return sequencerIoConfig().band[band].atu; }
  bool ah4Begin(uint32_t) override { return ::ah4Begin(); }
  bool ah4KeySeen() override { return ::ah4KeySeen(); }
  bool ah4KeyReleased() override { return ::ah4KeyReleased(); }
  Ah4Driver::Result ah4TakeResult() override { return ::ah4TakeResult(); }
  void ah4Abort(uint32_t) override { ::ah4Abort(); }
  bool ah4Busy() override { return ::ah4Busy(); }
};

RealEnv env;
TuneCycle cycle(env);
Blinker led; // the tune indicator pattern; sounded on the buzzer (D7)
bool buzzing = false;
TuneStep lastStep = TuneStep::Idle;
TuneOptions lastOptions = TUNE_FULL;
bool useAlc = false;        // key L toggles: inject ALC during tunes
uint8_t tuneMode = MODE_AM; // radio mode used for tuning; keys 1/2/3 change it

const __FlashStringHelper *stepName(TuneStep s) {
  switch (s) {
    case TuneStep::Claim: return F("claiming CAT bus");
    case TuneStep::CatOn: return F("CAT on");
    case TuneStep::QueryTx: return F("checking radio is receiving");
    case TuneStep::QueryRx: return F("reading RX status");
    case TuneStep::QueryFreq: return F("reading frequency and mode");
    case TuneStep::SeqUp: return F("sequencer up (tune profile)");
    case TuneStep::SetAm: return F("setting tune mode");
    case TuneStep::Alc: return F("ALC settling");
    case TuneStep::Ah4: return F("START/KEY handshake");
    case TuneStep::Dwell: return F("dry-run dwell");
    case TuneStep::TailPtt: return F("PTT off");
    case TuneStep::TailMode: return F("restoring mode");
    case TuneStep::TailSeq: return F("sequencer down");
    default: return F("idle");
  }
}

const __FlashStringHelper *reasonText(TuneReason r) {
  switch (r) {
    case TuneReason::BandBusy: return F("a band is already transmitting or sequencing");
    case TuneReason::RadioTransmitting: return F("the radio is transmitting");
    case TuneReason::BusTimeout: return F("could not claim the CAT bus");
    case TuneReason::CatFailed: return F("CAT query failed or gave an unusable answer");
    case TuneReason::NoBand: return F("frequency is not in any configured band");
    case TuneReason::BandUnsupported: return F("the ATU is not enabled for this band (config \"atu\")");
    case TuneReason::SequencerTimeout: return F("sequencer never came up");
    case TuneReason::Ah4Busy: return F("START still held from a previous cycle");
    case TuneReason::KeyStuck: return F("KEY already asserted before START");
    case TuneReason::NoAtu: return F("no ATU: KEY never asserted");
    case TuneReason::AtuTimeout: return F("KEY never released");
    case TuneReason::TuneFailed: return F("the ATU reported not tuned");
    case TuneReason::RfNotKeyed: return F("the radio was never keyed");
    case TuneReason::Watchdog: return F("cycle watchdog expired");
    case TuneReason::Aborted: return F("aborted");
    default: return F("");
  }
}

void printMHz(uint32_t hz) {
  Serial.print(hz / 1000000);
  Serial.print('.');
  uint32_t frac = hz % 1000000;
  for (uint32_t d = 100000; d > 0; d /= 10) Serial.print(frac / d % 10);
}

void begin(TuneOptions o) {
  o.mode = tuneMode;
  o.alc = useAlc;
  lastOptions = o;
  if (cycle.start(o, millis())) {
    Serial.println(o.carrier ? F("TUNE: started (carrier test: radio keyed for 2s, no ATU)")
                   : o.preKey ? F("TUNE: started (radio keyed at START, before KEY)")
                   : o.key   ? F("TUNE: started (radio keyed when KEY asserts)")
                   : o.ah4   ? F("TUNE: started (ATU handshake, radio NOT keyed)")
                             : F("TUNE: started (dry run)"));
  }
}

} // namespace

void tuneIoBegin() {
  buzzerBegin();
  led.set(Blinker::Pattern::Off, millis());
}

bool tuneIoActive() { return cycle.active(); }

void tuneIoButtonPress() {
  if (cycle.active()) {
    Serial.println(F("TUNE: abort requested"));
    cycle.abort(millis());
  } else {
    begin(TUNE_FULL);
  }
}

bool tuneIoHandleChar(char c) {
  switch (c) {
    case 'T': begin(TUNE_FULL); return true;
    case '1': tuneMode = MODE_AM; Serial.println(F("TUNE: tune mode AM")); return true;
    case '2': tuneMode = MODE_FM; Serial.println(F("TUNE: tune mode FM")); return true;
    case '3': tuneMode = MODE_CW; Serial.println(F("TUNE: tune mode CW (carrier may need a key closure)")); return true;
    case 'L':
      useAlc = !useAlc;
      Serial.println(useAlc ? F("TUNE: ALC injection ON for tunes") : F("TUNE: ALC injection off for tunes"));
      return true;
    case 'R': begin(TUNE_FULL_ONKEY); return true;
    case 'M': begin(TUNE_FULL_METER); return true;
    case 'P': begin(TUNE_CARRIER); return true;
    case 'E': begin(TUNE_ATU_NO_RF); return true;
    case 'D': begin(TUNE_DRY); return true;
    case 'X':
      Serial.println(F("TUNE: abort requested"));
      cycle.abort(millis());
      return true;
  }
  return false;
}

void tuneIoPoll() {
  uint32_t now = millis();
  cycle.poll(now);

  uint32_t after;
  uint8_t status;
  while (cycle.takeMeter(after, status)) {
    Serial.print(F("TUNE: meter +"));
    Serial.print(after);
    Serial.print(F("ms after PTT: PO/ALC "));
    Serial.print(status & 0x1F);
    Serial.print(F("/31, PTT "));
    Serial.println((status & 0x80) ? F("off") : F("on"));
  }

  TuneStep s = cycle.step();
  if (s != lastStep) {
    if (s != TuneStep::Idle) {
      Serial.print(F("TUNE: "));
      Serial.println(stepName(s));
    }
    if (s == TuneStep::SeqUp) { // frequency, mode and band are known by here
      Serial.print(F("TUNE: "));
      printMHz(cycle.freqHz());
      Serial.print(F(" MHz, mode 0x"));
      Serial.print(cycle.originalMode(), HEX);
      Serial.print(F(", band index "));
      Serial.println(cycle.band());
    }
    lastStep = s;
  }

  TuneOutcome o;
  TuneReason r;
  if (cycle.takeOutcome(o, r)) {
    switch (o) {
      case TuneOutcome::Success:
        Serial.println(lastOptions.carrier ? F("TUNE: carrier test complete")
                       : lastOptions.key   ? F("TUNE: success")
                                           : F("TUNE: finished OK (no RF was applied, so this was not a real tune)"));
        led.set(Blinker::Pattern::Success, now);
        break;
      case TuneOutcome::Aborted:
        Serial.println(F("TUNE: aborted, radio restored"));
        led.set(Blinker::Pattern::Off, now);
        break;
      default:
        Serial.print(o == TuneOutcome::Refused ? F("TUNE: refused: ") : F("TUNE: FAILED: "));
        Serial.println(reasonText(r));
        led.set(Blinker::Pattern::Failure, now);
        break;
    }
    if (cycle.modeRestoreFailed()) {
      Serial.println(F("TUNE: WARNING: could not restore the radio's original mode - check it!"));
    }
  }

  if (cycle.active()) {
    if (led.pattern() != Blinker::Pattern::On) led.set(Blinker::Pattern::On, now);
  } else if (led.finished(now)) {
    led.set(Blinker::Pattern::Off, now);
  }
  bool sound = led.level(now);
  if (sound != buzzing) {
    buzzing = sound;
    buzzerSet(sound);
  }
}
