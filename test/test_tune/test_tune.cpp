#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "cat_bridge_core.h"
#include "cat_codec.h"
#include "tune.h"

// ---- The world: real sequencer, arbiter and AH-4 driver, with a fake radio and a
// fake tuner around them. Stepped one millisecond at a time in the same order as
// the firmware's main loop (hardware, tune cycle, then the CAT bus). ----

struct Fake : TuneEnv, CatBridgeIo {
  SequencerConfig cfg;
  Sequencer seq;
  CatBridgeCore core;
  Ah4Driver ah4;

  // fake radio
  uint8_t mode = MODE_USB;
  uint32_t freq = 14250000;
  bool tx = false;
  uint8_t radioFrame[5] = {};
  uint8_t radioFrameLen = 0;
  struct Rx { uint32_t at; uint8_t b; };
  Rx rxq[512];
  uint32_t replyFreeAt = 0; // a real radio answers one command at a time
  uint8_t rxn = 0;
  uint32_t now_ = 0;
  uint8_t lastCmd[5] = {};
  int unknownFrames = 0; // radio saw a frame with an opcode it doesn't know: bytes misaligned
  int pcFramesAtRadio = 0;

  // fake PC: bytes scheduled to arrive on the PC port, and everything the bridge wrote to it
  struct PcIn { uint32_t at; uint8_t b; };
  PcIn pcq[1024];
  uint16_t pcqHead = 0, pcqTail = 0;
  struct PcOut { uint32_t at; uint8_t b; };
  PcOut pcOut[2048];
  uint16_t pcOutN = 0;
  bool pcNeverPauses = false, radioMute = false;
  int synth = 0, swallowed = 0, queuedFrames = 0, replayed = 0;
  uint32_t holdStart = 0, holdEnd = 0, interceptStart = 0, interceptEnd = 0;
  struct ModeSet { uint32_t at; uint8_t mode; };
  ModeSet modeSets[32];
  uint8_t modeSetN = 0;

  // fake tuner (Alinco EDX-2 timing, or genuine AH-4 with KEY during the START hold)
  bool genuine = false;
  bool noAtu = false, keyStuck = false, neverRelease = false, failTune = false;
  uint32_t tuneMs = 800;
  bool prevStart = false;
  bool armed = false, keyOn = false, rfSeen = false;
  uint32_t armAt = 0, keyOnAt = 0, rfAt = 0;
  uint8_t failPhase = 0; // not-tuned signature: 1 gap, 2 reasserted
  uint32_t phaseAt = 0;

  // injection
  bool rfi430 = false; // spurious STBY pulses on 430M while the radio transmits
  uint32_t rfiAt = 0;
  int inhibitGlitches = 0;
  bool band3Ever = false;
  bool dropF7 = false, dropFreq = false, badMode = false, seqStuck = false;
  bool blockQueries = false, failPttOn = false;
  int failPttOffTimes = 0, failRestoreTimes = 0;
  uint32_t outOfBandFreq = 0;

  // observations
  uint32_t startAt = 0, keyAssertAt = 0, keyReleaseAt = 0, pttOnAt = 0, pttOffAt = 0;
  uint32_t seqActiveAt = 0, amSetAt = 0, restoreAt = 0, claimReleasedAt = 0;
  int pttOnCount = 0, pttOffCount = 0;
  bool recArmed = false;
  uint8_t recMode = 0xFF;
  int armCount = 0, disarmCount = 0;
  uint32_t recArmAt = 0, recDisarmAt = 0;
  int ev = 0; // strict order of events, finer than a millisecond
  int evArm = 0, evDisarm = 0, evAmSet = 0, evPttOn = 0, evPttOff = 0, evRestore = 0;
  uint8_t modeAtPttOn = 0xFF;
  bool hold[SEQ_BANDS] = {};
  bool seq3EverOnHf = false;
  bool inhibitOnDuringRf = false;

  Fake() : seq(cfg), core(*this) {
    cfg = DEFAULT_SEQUENCER_CONFIG;
    cfg.band[0].gapMs[0] = 50; cfg.band[0].gapMs[1] = 60; cfg.band[0].gapMs[2] = 20;
  }

  // ---- the PC, scripted ----
  void pcSend(const uint8_t *bytes, uint8_t n, uint32_t at) { // bytes arrive 1ms apart
    for (uint8_t i = 0; i < n; i++) pcq[pcqTail++ % 1024] = {at + i, bytes[i]};
  }
  void pcSendFrame(uint8_t p0, uint8_t op, uint32_t at) {
    const uint8_t f[5] = {p0, 0, 0, 0, op};
    pcSend(f, 5, at);
  }

  // ---- CatBridgeIo ----
  int pcRead() override {
    if (pcqHead != pcqTail && pcq[pcqHead % 1024].at <= now_) return pcq[pcqHead++ % 1024].b;
    return -1;
  }
  int radioRead() override {
    for (uint8_t i = 0; i < rxn; i++) {
      if (now_ >= rxq[i].at) {
        uint8_t b = rxq[i].b;
        rxq[i] = rxq[--rxn];
        return b;
      }
    }
    return -1;
  }
  void pcWrite(uint8_t b) override { pcOut[pcOutN++] = {now_, b}; }
  void radioWrite(uint8_t b) override {
    radioFrame[radioFrameLen++] = b;
    if (radioFrameLen == 5) {
      radioFrameLen = 0;
      radioHandle(radioFrame, now_);
    }
  }
  void frame(Frame k, const uint8_t *, uint8_t) override {
    if (k == Frame::SynthToPc) synth++;
    if (k == Frame::SwallowedFromPc) swallowed++;
    if (k == Frame::QueuedFromPc) queuedFrames++;
    if (k == Frame::Replayed) replayed++;
  }

  // ---- TuneEnv ----
  bool catClaim(uint32_t now) override { return core.claim(now); }
  CatArbiter::ClaimState catClaimState() override { return core.claimState(); }
  void catRelease(uint32_t now) override { core.releaseClaim(now); claimReleasedAt = now; }
  void catSetSnapshot(const CatSnapshot &s) override { core.setSnapshot(s); }
  void recoveryArm(uint8_t m) override { recArmed = true; recMode = m; armCount++; recArmAt = now_; evArm = ++ev; }
  void recoveryDisarm() override { recArmed = false; disarmCount++; recDisarmAt = now_; evDisarm = ++ev; }
  bool catSubmit(const uint8_t cmd[5], uint32_t now) override {
    if (blockQueries && (cmd[4] == 0xF7 || cmd[4] == 0x03)) return false;
    if (!core.submit(cmd, now)) return false;
    memcpy(lastCmd, cmd, 5);
    return true;
  }
  bool catTakeResult(CatArbiter::Result &r, uint8_t *reply, uint8_t &len) override {
    if (!core.takeResult(r, reply, len)) return false;
    if (lastCmd[4] == 0x08 && failPttOn) r = CatArbiter::Result::NoReply;
    if (lastCmd[4] == 0x88 && failPttOffTimes > 0) { failPttOffTimes--; r = CatArbiter::Result::NoReply; }
    if (lastCmd[4] == 0x07 && lastCmd[0] != MODE_AM && failRestoreTimes > 0) {
      failRestoreTimes--; r = CatArbiter::Result::NoReply;
    }
    return true;
  }
  bool bandsAllIdle() override { return seq.allIdle(); }
  void seqHold(uint8_t band, bool held) override { hold[band] = held; seq.setHold(band, held); }
  void seqRequest(uint8_t band, bool wantTx, uint32_t now) override {
    seq.request(band, wantTx, Profile::Tune, now);
  }
  bool seqActive(uint8_t band) override { return !seqStuck && seq.active(band); }
  bool seqIdle(uint8_t band) override { return seq.idle(band); }
  int8_t bandForFreq(uint32_t hz) override { return bandForFrequency(cfg, hz); }
  bool atuAllowed(uint8_t band) override { return cfg.band[band].atu; }
  bool ah4Begin(uint32_t now) override {
    startAt = now;
    return ah4.begin(keyLine(), now);
  }
  bool ah4KeySeen() override { return ah4.keySeen(); }
  bool ah4KeyReleased() override { return ah4.keyReleased(); }
  Ah4Driver::Result ah4TakeResult() override { return ah4.takeResult(); }
  void ah4Abort(uint32_t now) override { ah4.abort(now); }
  bool ah4Busy() override { return ah4.busy(); }

  // ---- the world ----
  bool keyLine() const { return keyOn || keyStuck; }

  void radioHandle(const uint8_t *c, uint32_t now) {
    uint8_t op = c[4];
    auto reply = [&](const uint8_t *b, uint8_t n) {
      if (radioMute) return;
      uint32_t t0 = now + 3 > replyFreeAt ? now + 3 : replyFreeAt;
      for (uint8_t i = 0; i < n; i++) {
        TEST_ASSERT_LESS_THAN_MESSAGE(500, rxn, "fake radio reply buffer overflow");
        rxq[rxn++] = {t0 + i, b[i]};
      }
      replyFreeAt = t0 + n;
    };
    static const uint8_t known[] = {0x00, 0x80, 0x01, 0x03, 0x07, 0x08, 0x88, 0xE7, 0xF7, 0x09, 0x0A,
                                    0x11, 0x13, 0x17, 0x21, 0x23, 0x27, 0x4E, 0x8E, 0xF9};
    bool ok = false;
    for (uint8_t k : known) ok |= k == op;
    if (!ok) unknownFrames++;
    if (op == 0xF7) {
      uint8_t s = tx ? 0x00 : 0x80;
      if (!dropF7) reply(&s, 1);
    } else if (op == 0xE7) {
      uint8_t s = 0x1F;
      reply(&s, 1);
    } else if (op == 0x03) {
      uint8_t r[5];
      catEncodeFreq(freq, r);
      r[4] = badMode ? 0x55 : mode;
      if (!dropFreq) reply(r, 5);
    } else if (op == 0x01) {
      uint32_t hz;
      if (catDecodeFreq(c, hz)) freq = hz;
    } else if (op == 0x13) { // sat RX freq+mode: answered like main, for the "unanswerable" test
      uint8_t r[5];
      catEncodeFreq(freq, r);
      r[4] = mode;
      reply(r, 5);
    } else if (op == 0x07) {
      mode = c[0];
      if (modeSetN < 32) modeSets[modeSetN++] = {now, c[0]};
      if (c[0] == MODE_AM && amSetAt == 0) { amSetAt = now; evAmSet = ++ev; }
      if (c[0] != MODE_AM) { restoreAt = now; evRestore = ++ev; }
    } else if (op == 0x08) {
      tx = true;
      pttOnCount++;
      if (pttOnCount == 1) { pttOnAt = now; modeAtPttOn = mode; evPttOn = ++ev; }
      seq.stby(0, true, now); // the real radio asserts STBY when keyed (HF here)
    } else if (op == 0x88) {
      tx = false;
      pttOffCount++;
      pttOffAt = now;
      evPttOff = ++ev;
      seq.stby(0, false, now);
    }
  }

  void tunerStep(uint32_t now) {
    bool startOn = ah4.startAsserted();
    if (genuine) {
      if (startOn && !prevStart) { armed = true; armAt = now + 300; }
    } else if (!startOn && prevStart) {
      armed = true;
      armAt = now + 31; // KEY ~31ms after START is released
    }
    prevStart = startOn;
    if (armed && !keyOn && !noAtu && now >= armAt && failPhase == 0) {
      keyOn = true; keyOnAt = now; armed = false; rfSeen = false;
      keyAssertAt = now;
    }
    if (failPhase == 1 && now >= phaseAt + 20) { // not-tuned signature: KEY back for 200ms
      keyOn = true;
      failPhase = 2;
      phaseAt = now;
    } else if (failPhase == 2 && now >= phaseAt + 200) {
      keyOn = false;
      failPhase = 0;
    } else if (failPhase == 0 && keyOn) {
      if (!rfSeen && tx) { rfSeen = true; rfAt = now; }
      if (rfSeen && tx && seq.outputs().txInhibit) inhibitOnDuringRf = true;
      if (!rfSeen && now >= keyOnAt + 326) { keyOn = false; keyReleaseAt = now; }
      else if (rfSeen && !neverRelease && now >= rfAt + tuneMs) {
        keyOn = false;
        keyReleaseAt = now;
        if (failTune) { failPhase = 1; phaseAt = now; }
      }
    }
  }

  void hw(uint32_t now) {
    now_ = now;
    tunerStep(now);
    ah4.poll(keyLine(), now);
    if (rfi430 && tx) { // 7ms pulses on the 430M STBY line, as seen on the bench
      uint32_t phase = (now - rfiAt) % 20;
      if (phase == 0) seq.stby(3, true, now);
      if (phase == 7) seq.stby(3, false, now);
    }
    seq.poll(now);
    if (seq.outputs().seq[0][2]) seq3EverOnHf = true;
    if (tx && seq.outputs().txInhibit) inhibitGlitches++;
    if (!seq.idle(3)) band3Ever = true;
  }

  void bus(uint32_t now) { // the CAT bridge
    now_ = now;
    if (pcNeverPauses && now % 400 == 1) pcSendFrame(0, 0x03, now); // a PC that never goes quiet
    core.poll(now);
    CatBridgeCore::Mode m = core.mode();
    if (m == CatBridgeCore::Mode::Hold && !holdStart) holdStart = now;
    if (m != CatBridgeCore::Mode::Hold && holdStart && !holdEnd) holdEnd = now;
    if (m == CatBridgeCore::Mode::Intercept && !interceptStart) interceptStart = now;
    if (m != CatBridgeCore::Mode::Intercept && interceptStart && !interceptEnd) interceptEnd = now;
  }
};

// Run until the cycle has unwound; optionally abort at a given ms; returns end time.
static uint32_t runCycle(Fake &f, TuneCycle &c, TuneOptions o, int32_t abortAt = -1,
                         uint32_t maxMs = 60000) {
  c.start(o, 0);
  for (uint32_t t = 1; t < maxMs; t++) {
    f.hw(t);
    if (abortAt >= 0 && t == (uint32_t)abortAt) c.abort(t);
    c.poll(t);
    f.bus(t);
    if (!c.active()) {
      for (uint32_t u = t + 1; u < t + 800; u++) { f.hw(u); f.bus(u); } // let START finish its hold
      return t;
    }
  }
  TEST_FAIL_MESSAGE("cycle never unwound");
  return maxMs;
}

// Whatever happened, the system must be left safe.
static void expectSafe(Fake &f, uint8_t originalMode) {
  TEST_ASSERT_FALSE_MESSAGE(f.tx, "radio left transmitting");
  TEST_ASSERT_FALSE_MESSAGE(f.recArmed, "crash-recovery record left armed");
  TEST_ASSERT_EQUAL_MESSAGE(f.armCount, f.disarmCount, "recovery armed and disarmed a different number of times");
  TEST_ASSERT_EQUAL_MESSAGE(f.pttOnCount > 0 ? 1 : 0, f.pttOnCount > 0 ? f.pttOffCount > 0 : 0,
                            "PTT on without a later PTT off");
  TEST_ASSERT_EQUAL_MESSAGE(CatArbiter::ClaimState::None, f.core.claimState(), "CAT bus still claimed");
  TEST_ASSERT_FALSE_MESSAGE(f.ah4.busy(), "AH-4 START still held");
  TEST_ASSERT_FALSE_MESSAGE(f.ah4.startAsserted(), "START asserted");
  TEST_ASSERT_TRUE_MESSAGE(f.seq.allIdle(), "sequencer not idle");
  TEST_ASSERT_FALSE_MESSAGE(f.seq.outputs().txInhibit, "TX INHIBIT left on");
  for (uint8_t b = 0; b < SEQ_BANDS; b++) TEST_ASSERT_FALSE_MESSAGE(f.hold[b], "STBY hold left on");
  TEST_ASSERT_EQUAL_MESSAGE(originalMode, f.mode, "radio mode not restored");
}

static TuneOutcome outcomeOf(TuneCycle &c, TuneReason &why) {
  TuneOutcome o = TuneOutcome::None;
  TEST_ASSERT_TRUE(c.takeOutcome(o, why));
  TuneOutcome again;
  TuneReason r2;
  TEST_ASSERT_FALSE(c.takeOutcome(again, r2)); // delivered once
  return o;
}

void setUp() {}

// ---- the happy path ----

void test_full_tune_on_an_edx2_style_tuner() {
  Fake f;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  expectSafe(f, MODE_USB);
  TEST_ASSERT_EQUAL(MODE_AM, f.modeAtPttOn);
  TEST_ASSERT_EQUAL(1, f.pttOnCount);
  TEST_ASSERT_EQUAL(1, f.pttOffCount);
  // order: START only after the sequencer is up and AM is set
  TEST_ASSERT_GREATER_THAN(f.amSetAt, f.startAt);
  // PTT goes out the instant KEY asserts, and off the instant it releases
  TEST_ASSERT_LESS_OR_EQUAL(3, f.pttOnAt - f.keyAssertAt);
  TEST_ASSERT_LESS_OR_EQUAL(3, f.pttOffAt - f.keyReleaseAt);
  TEST_ASSERT_LESS_THAN(AH4_CONFIRM_MS, f.pttOffAt - f.keyReleaseAt);
  // mode restored after PTT off; claim released after the restore
  TEST_ASSERT_GREATER_THAN(f.pttOffAt, f.restoreAt);
  TEST_ASSERT_GREATER_OR_EQUAL(f.restoreAt, f.claimReleasedAt);
  // the radio's STBY during PTT must not have run the normal profile on HF
  TEST_ASSERT_FALSE_MESSAGE(f.seq3EverOnHf, "STBY ran the normal profile during the tune");
  TEST_ASSERT_FALSE(f.inhibitOnDuringRf);
}

void test_full_tune_on_a_genuine_ah4_style_tuner() {
  Fake f;
  f.genuine = true;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  expectSafe(f, MODE_USB);
  TEST_ASSERT_LESS_OR_EQUAL(3, f.pttOnAt - f.keyAssertAt);
  TEST_ASSERT_LESS_OR_EQUAL(3, f.pttOffAt - f.keyReleaseAt);
}

void test_recovery_record_brackets_every_change_to_the_radio() {
  Fake f;
  f.mode = MODE_CWN; // a narrow mode, to prove the exact byte is recorded
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(1, f.armCount);
  TEST_ASSERT_EQUAL(1, f.disarmCount);
  TEST_ASSERT_EQUAL(MODE_CWN, f.recMode);
  TEST_ASSERT_LESS_THAN(f.evAmSet, f.evArm);         // armed before the mode is changed...
  TEST_ASSERT_LESS_THAN(f.evPttOn, f.evArm);         // ...and before the radio is keyed
  TEST_ASSERT_GREATER_THAN(f.evPttOff, f.evDisarm);  // disarmed only after PTT off...
  TEST_ASSERT_GREATER_THAN(f.evRestore, f.evDisarm); // ...and after the mode is restored
  expectSafe(f, MODE_CWN);
}

void test_refusals_before_the_radio_is_touched_never_arm_the_record() {
  { Fake f; f.tx = true; TuneCycle c(f); runCycle(f, c, TUNE_FULL); TEST_ASSERT_EQUAL(0, f.armCount); }
  { Fake f; f.freq = 145500000; TuneCycle c(f); runCycle(f, c, TUNE_FULL); TEST_ASSERT_EQUAL(0, f.armCount); }
  { Fake f; f.dropFreq = true; TuneCycle c(f); runCycle(f, c, TUNE_FULL); TEST_ASSERT_EQUAL(0, f.armCount); }
  { Fake f; f.seqStuck = true; TuneCycle c(f); runCycle(f, c, TUNE_FULL); TEST_ASSERT_EQUAL(0, f.armCount); }
}

void test_a_stuck_ptt_off_keeps_the_record_armed_until_it_works() {
  Fake f;
  f.failPttOffTimes = 1000; // the radio never acknowledges PTT off
  TuneCycle c(f);
  c.start(TUNE_FULL, 0);
  for (uint32_t t = 1; t < 12000; t++) { f.hw(t); c.poll(t); f.bus(t); }
  TEST_ASSERT_TRUE(c.active());          // still trying to unkey
  TEST_ASSERT_TRUE_MESSAGE(f.recArmed, "must stay armed while the radio may still be keyed");
}

void test_already_in_am_is_left_alone() {
  Fake f;
  f.mode = MODE_AM;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(0, f.amSetAt); // never sent a mode change
  TEST_ASSERT_EQUAL(0, f.restoreAt);
  expectSafe(f, MODE_AM);
}

void test_narrow_mode_restored_exactly() {
  Fake f;
  f.mode = MODE_CWN;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  expectSafe(f, MODE_CWN);
}

void test_spurious_stby_on_another_band_cannot_disturb_the_tune() {
  Fake f;
  f.rfi430 = true;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  TEST_ASSERT_EQUAL_MESSAGE(0, f.inhibitGlitches, "TX INHIBIT glitched during the carrier");
  TEST_ASSERT_FALSE_MESSAGE(f.band3Ever, "430M sequencer started from pickup");
  expectSafe(f, MODE_USB);
}

void test_a_long_tune_is_allowed() {
  // The EDX-2 was still tuning at 2.5s: a 9s tune must complete, with PTT held throughout.
  Fake f;
  f.tuneMs = 9000;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  TEST_ASSERT_GREATER_THAN(8000, f.pttOffAt - f.pttOnAt);
  expectSafe(f, MODE_USB);
}

void test_meter_diagnostic_polls_without_breaking_the_unkey() {
  Fake f;
  TuneCycle c(f);
  c.start(TUNE_FULL_METER, 0);
  int samples = 0;
  uint32_t maxUnkeyDelay = 0;
  for (uint32_t t = 1; t < 30000 && c.active(); t++) {
    f.hw(t);
    c.poll(t);
    f.bus(t);
    uint32_t after;
    uint8_t status;
    while (c.takeMeter(after, status)) {
      samples++;
      TEST_ASSERT_EQUAL_MESSAGE(0, status & 0x80, "radio should report PTT on while keyed");
    }
  }
  maxUnkeyDelay = f.pttOffAt - f.keyReleaseAt;
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  TEST_ASSERT_GREATER_THAN(5, samples);          // ~800ms tune, a sample every ~60ms
  TEST_ASSERT_LESS_OR_EQUAL(110, maxUnkeyDelay); // a query in flight + the 50ms bus spacing; bounded
  for (uint32_t u = 30000; u < 30800; u++) { f.hw(u); f.bus(u); }
  expectSafe(f, MODE_USB);
}

void test_meter_is_off_in_a_normal_tune() {
  Fake f;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  uint32_t after;
  uint8_t status;
  TEST_ASSERT_FALSE(c.takeMeter(after, status)); // no extra bus traffic unless asked for
  TEST_ASSERT_LESS_OR_EQUAL(3, f.pttOffAt - f.keyReleaseAt);
}

void test_carrier_diagnostic_keys_the_radio_alone() {
  Fake f;
  TuneCycle c(f);
  c.start(TUNE_CARRIER, 0);
  int samples = 0;
  uint32_t t = 1;
  for (; t < 30000 && c.active(); t++) {
    f.hw(t);
    c.poll(t);
    f.bus(t);
    uint32_t after;
    uint8_t status;
    while (c.takeMeter(after, status)) samples++;
  }
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(0, f.startAt);             // no tuner involved
  TEST_ASSERT_EQUAL(1, f.pttOnCount);
  TEST_ASSERT_EQUAL(MODE_AM, f.modeAtPttOn);   // keyed in AM
  TEST_ASSERT_GREATER_THAN(20, samples);       // ~2s of samples
  TEST_ASSERT_GREATER_OR_EQUAL(TUNE_CARRIER_MS - 200, f.pttOffAt - f.pttOnAt);
  TEST_ASSERT_LESS_OR_EQUAL(TUNE_CARRIER_MS + 300, f.pttOffAt - f.pttOnAt);
  for (uint32_t u = t; u < t + 800; u++) { f.hw(u); f.bus(u); }
  expectSafe(f, MODE_USB);
}

void test_abort_during_the_carrier_test_unkeys() {
  for (int32_t at = 200; at < 2600; at += 37) {
    Fake f;
    TuneCycle c(f);
    runCycle(f, c, TUNE_CARRIER, at);
    char msg[64];
    snprintf(msg, sizeof msg, "unsafe after abort at %d ms", (int)at);
    TEST_ASSERT_FALSE_MESSAGE(f.tx, msg);
    TEST_ASSERT_EQUAL_MESSAGE(MODE_USB, f.mode, msg);
    TEST_ASSERT_EQUAL_MESSAGE(CatArbiter::ClaimState::None, f.core.claimState(), msg);
  }
}

void test_prekey_keys_before_the_tuner_asks_and_unkeys_at_key_release() {
  for (int genuine = 0; genuine < 2; genuine++) {
    Fake f;
    f.genuine = genuine;
    TuneCycle c(f);
    runCycle(f, c, TUNE_FULL);
    TuneReason why;
    TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
    TEST_ASSERT_LESS_THAN(f.keyAssertAt, f.pttOnAt);               // keyed before KEY
    TEST_ASSERT_GREATER_OR_EQUAL(f.startAt, f.pttOnAt);            // but not before START
    TEST_ASSERT_LESS_OR_EQUAL(3, f.pttOffAt - f.keyReleaseAt);     // still unkeyed at KEY's release
    TEST_ASSERT_EQUAL(1, f.pttOnCount);
    expectSafe(f, MODE_USB);
  }
}

void test_prekey_with_no_tuner_still_unkeys() {
  Fake f;
  f.noAtu = true;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Failed, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(TuneReason::NoAtu, why);
  expectSafe(f, MODE_USB); // the radio was keyed at START and must come off
}

void test_prekey_abort_sweep() {
  for (int32_t at = 1; at < 3200; at += 13) {
    Fake f;
    TuneCycle c(f);
    runCycle(f, c, TUNE_FULL, at);
    char msg[64];
    snprintf(msg, sizeof msg, "unsafe after abort at %d ms", (int)at);
    TEST_ASSERT_FALSE_MESSAGE(f.tx, msg);
    TEST_ASSERT_EQUAL_MESSAGE(CatArbiter::ClaimState::None, f.core.claimState(), msg);
    TEST_ASSERT_FALSE_MESSAGE(f.ah4.busy(), msg);
    TEST_ASSERT_EQUAL_MESSAGE(MODE_USB, f.mode, msg);
  }
}

void test_tune_mode_is_selectable_and_restored() {
  Fake f;
  TuneCycle c(f);
  TuneOptions o = TUNE_FULL;
  o.mode = MODE_FM;
  runCycle(f, c, o);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(MODE_FM, f.modeAtPttOn);
  expectSafe(f, MODE_USB);
}

void test_default_is_to_key_at_start() {
  TEST_ASSERT_TRUE(TUNE_FULL.preKey);
  TEST_ASSERT_FALSE(TUNE_FULL_ONKEY.preKey);
}

// The same failure scenarios, on the default (key at START) path.
void test_default_path_failures_all_end_safe() {
  struct Scn { const char *name; void (*set)(Fake &); TuneOutcome o; TuneReason r; };
  static const Scn scns[] = {
    {"key stuck",    [](Fake &f) { f.keyStuck = true; },     TuneOutcome::Failed, TuneReason::KeyStuck},
    {"never releases", [](Fake &f) { f.neverRelease = true; }, TuneOutcome::Failed, TuneReason::AtuTimeout},
    {"not tuned",    [](Fake &f) { f.failTune = true; },     TuneOutcome::Failed, TuneReason::TuneFailed},
    {"ptt on fails", [](Fake &f) { f.failPttOn = true; },    TuneOutcome::Failed, TuneReason::CatFailed},
    {"ptt off retried", [](Fake &f) { f.failPttOffTimes = 3; }, TuneOutcome::Success, TuneReason::None},
    {"rfi on 430M",  [](Fake &f) { f.rfi430 = true; },       TuneOutcome::Success, TuneReason::None},
  };
  for (const Scn &sc : scns) {
    Fake f;
    sc.set(f);
    TuneCycle c(f);
    runCycle(f, c, TUNE_FULL);
    TuneReason why;
    TuneOutcome o = outcomeOf(c, why);
    char msg[64];
    snprintf(msg, sizeof msg, "scenario '%s'", sc.name);
    TEST_ASSERT_EQUAL_MESSAGE((int)sc.o, (int)o, msg);
    TEST_ASSERT_EQUAL_MESSAGE((int)sc.r, (int)why, msg);
    expectSafe(f, MODE_USB);
  }
}


// ---- Phase 8: the PC must not be able to tell a tune is happening ----

// A PC polling freq+mode and TX status; returns the bytes it must be given back, in
// order. The radio's state as the PC knows it never changes (USB, 14.25MHz, receiving),
// so the answers must be identical before, during and after the tune.
struct Poll { uint32_t at; bool freq; };
static void schedulePolling(Fake &f, uint32_t from, uint32_t to, uint32_t period,
                            uint8_t *expect, uint16_t &expectN, Poll *polls, uint16_t &pollN) {
  bool freqQuery = true;
  for (uint32_t t = from; t < to; t += period) {
    f.pcSendFrame(0, freqQuery ? 0x03 : 0xF7, t);
    polls[pollN++] = {t, freqQuery};
    if (freqQuery) {
      uint8_t r[5];
      catEncodeFreq(14250000, r);
      for (int i = 0; i < 4; i++) expect[expectN++] = r[i];
      expect[expectN++] = MODE_USB; // the ORIGINAL mode, never AM
    } else {
      expect[expectN++] = 0x80; // receiving, never "transmitting"
    }
    freqQuery = !freqQuery;
  }
}

static void flush(Fake &f, uint32_t from, uint32_t ms) {
  for (uint32_t u = from; u < from + ms; u++) { f.hw(u); f.bus(u); }
}
static void flushUntil(Fake &f, uint32_t from, uint32_t until) { // an aborted cycle ends early
  if (until > from) flush(f, from, until - from);
}

static void expectPcStream(Fake &f, const uint8_t *expect, uint16_t expectN) {
  TEST_ASSERT_EQUAL_MESSAGE(expectN, f.pcOutN, "PC got a different number of reply bytes");
  for (uint16_t i = 0; i < expectN; i++) {
    if (f.pcOut[i].b != expect[i]) {
      char msg[96];
      snprintf(msg, sizeof msg, "PC reply byte %u differs: got %02X want %02X (t=%u)", (unsigned)i,
               f.pcOut[i].b, expect[i], (unsigned)f.pcOut[i].at);
      TEST_FAIL_MESSAGE(msg);
    }
  }
}

void test_pc_polling_through_a_whole_tune_sees_no_difference() {
  Fake f;
  uint8_t expect[400];
  uint16_t en = 0, pn = 0;
  Poll polls[200];
  schedulePolling(f, 20, 3600, 37, expect, en, polls, pn); // before, during and after
  TuneCycle c(f);
  uint32_t end = runCycle(f, c, TUNE_FULL);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  flush(f, end + 801, 1500);
  expectPcStream(f, expect, en); // every reply right, in order; no AM mode, no "transmitting"
  expectSafe(f, MODE_USB);
  TEST_ASSERT_EQUAL(1, f.pttOnCount);
  TEST_ASSERT_GREATER_THAN(10, f.synth); // most of it was answered from the snapshot
  TEST_ASSERT_EQUAL(CatBridgeCore::Mode::Normal, f.core.mode());
  TEST_ASSERT_EQUAL(0, f.core.queued());
  TEST_ASSERT_EQUAL(0, f.unknownFrames);
  // the bridge only holds the PC for a short while at the start of the tune
  TEST_ASSERT_LESS_THAN(500, f.holdEnd - f.holdStart);
}

void test_replies_during_the_tune_are_immediate() {
  Fake f;
  uint8_t expect[400];
  uint16_t en = 0, pn = 0;
  Poll polls[200];
  schedulePolling(f, 20, 3600, 37, expect, en, polls, pn);
  TuneCycle c(f);
  uint32_t end = runCycle(f, c, TUNE_FULL);
  flush(f, end + 801, 1500);
  TEST_ASSERT_GREATER_THAN(0, f.interceptStart);
  TEST_ASSERT_GREATER_THAN(f.interceptStart, f.interceptEnd);
  // each query is 5 bytes arriving 1ms apart; its reply must follow within a few ms
  uint16_t at = 0;
  int checked = 0;
  for (uint16_t i = 0; i < pn; i++) {
    uint8_t n = polls[i].freq ? 5 : 1;
    uint32_t queryEnd = polls[i].at + 4;
    uint32_t replyEnd = f.pcOut[at + n - 1].at;
    at += n;
    if (queryEnd > f.interceptStart + 5 && queryEnd + 10 < f.interceptEnd) {
      TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(8, replyEnd - queryEnd, "reply to the PC was late during the tune");
      checked++;
    }
  }
  TEST_ASSERT_GREATER_THAN(20, checked);
}

void test_pc_ptt_commands_cannot_key_or_unkey_the_radio() {
  Fake f;
  f.pcSendFrame(0, 0x08, 800);  // PC says PTT on, mid-tune
  f.pcSendFrame(0, 0x88, 1000); // ...and off
  f.pcSendFrame(0, 0x08, 1200);
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  TEST_ASSERT_EQUAL_MESSAGE(1, f.pttOnCount, "only the tune cycle may key the radio");
  TEST_ASSERT_EQUAL(1, f.pttOffCount);
  TEST_ASSERT_EQUAL(3, f.swallowed);
  TEST_ASSERT_EQUAL(0, f.queuedFrames); // not replayed afterwards either
  expectSafe(f, MODE_USB);
}

void test_pc_mode_change_during_the_tune_is_applied_after_the_restore() {
  Fake f;
  f.pcSendFrame(MODE_LSB, 0x07, 900); // the user changes mode in flrig mid-tune
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(1, f.queuedFrames);
  TEST_ASSERT_EQUAL(1, f.replayed);
  TEST_ASSERT_EQUAL_MESSAGE(MODE_LSB, f.mode, "the PC's mode change was lost");
  // order on the radio: AM for the tune, the Arduino's restore to USB, then the PC's LSB
  TEST_ASSERT_EQUAL(3, f.modeSetN);
  TEST_ASSERT_EQUAL(MODE_AM, f.modeSets[0].mode);
  TEST_ASSERT_EQUAL(MODE_USB, f.modeSets[1].mode);
  TEST_ASSERT_EQUAL(MODE_LSB, f.modeSets[2].mode);
  TEST_ASSERT_GREATER_THAN(f.modeSets[1].at, f.modeSets[2].at);
  TEST_ASSERT_FALSE(f.tx);
  TEST_ASSERT_EQUAL(0, f.unknownFrames);
}

void test_pc_frequency_change_is_replayed_and_the_tune_still_used_the_old_one() {
  Fake f;
  uint8_t fcmd[5];
  catCmdSetFreq(fcmd, 14200000);
  f.pcSend(fcmd, 5, 900);
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  TEST_ASSERT_EQUAL_UINT32(14289080 - 39080 /* 14.25MHz */, 14250000);
  TEST_ASSERT_EQUAL_UINT32(14200000, f.freq); // the PC's new frequency arrived afterwards
  TEST_ASSERT_EQUAL(14250000, c.freqHz());     // the tune was on the frequency it started on
}

void test_queued_commands_replay_in_order_and_overflow_keeps_the_newest() {
  Fake f;
  for (int i = 0; i < 12; i++) { // more than the queue holds
    uint8_t fcmd[5];
    catCmdSetFreq(fcmd, 14200000 + i * 1000);
    f.pcSend(fcmd, 5, 800 + i * 20);
  }
  TuneCycle c(f);
  uint32_t end = runCycle(f, c, TUNE_FULL);
  flush(f, end + 801, 2000);
  TEST_ASSERT_EQUAL(12 - CAT_REPLAY_MAX, f.core.queueDropped());
  TEST_ASSERT_EQUAL_UINT32(14200000 + 11 * 1000, f.freq); // the newest request won
  TEST_ASSERT_EQUAL(CatBridgeCore::Mode::Normal, f.core.mode());
}

void test_a_query_the_bridge_cannot_answer_is_delayed_not_lost() {
  Fake f;
  f.pcSendFrame(0, 0x13, 900); // sat RX freq/mode: not in the snapshot
  TuneCycle c(f);
  uint32_t end = runCycle(f, c, TUNE_FULL);
  flush(f, end + 801, 1500);
  TEST_ASSERT_EQUAL(1, f.queuedFrames);
  TEST_ASSERT_EQUAL(5, f.pcOutN); // it was answered, live, once the tune was over
  TEST_ASSERT_GREATER_OR_EQUAL(f.interceptEnd, f.pcOut[0].at); // not before the tune is over
  TEST_ASSERT_EQUAL(MODE_USB, f.pcOut[4].b);
}

void test_a_command_split_across_every_phase_is_never_cut_in_half() {
  // A mode-set from the PC whose bytes straddle the start of the claim, the Hold ->
  // Intercept switch, the end of the tune and the end of the drain, whichever way they fall.
  for (uint32_t T = 0; T < 4200; T += 29) {
    Fake f;
    const uint8_t cmd[5] = {MODE_LSB, 0, 0, 0, 0x07};
    f.pcSend(cmd, 3, T);        // first three bytes...
    f.pcSend(cmd + 3, 2, T + 45); // ...the rest 45ms later
    TuneCycle c(f);
    uint32_t end = runCycle(f, c, TUNE_FULL);
    flush(f, end + 801, 2000);
    char msg[80];
    snprintf(msg, sizeof msg, "split at %u ms", (unsigned)T);
    TEST_ASSERT_EQUAL_MESSAGE(0, f.unknownFrames, msg);          // no misaligned frame at the radio
    TEST_ASSERT_EQUAL_MESSAGE(MODE_LSB, f.mode, msg);            // the PC's change always lands
    TEST_ASSERT_FALSE_MESSAGE(f.tx, msg);
    TEST_ASSERT_EQUAL_MESSAGE(1, f.pttOnCount, msg);
    TEST_ASSERT_EQUAL_MESSAGE(CatBridgeCore::Mode::Normal, f.core.mode(), msg);
    TEST_ASSERT_EQUAL_MESSAGE(0, f.core.queued(), msg);
  }
}

void test_abort_with_a_polling_pc_leaves_the_pc_stream_intact() {
  for (int32_t at = 50; at < 3200; at += 211) {
    Fake f;
    uint8_t expect[400];
    uint16_t en = 0, pn = 0;
    Poll polls[200];
    schedulePolling(f, 20, 3600, 37, expect, en, polls, pn);
    TuneCycle c(f);
    uint32_t end = runCycle(f, c, TUNE_FULL, at);
    flushUntil(f, end + 801, 5000);
    char msg[80];
    snprintf(msg, sizeof msg, "abort at %d ms", (int)at);
    TEST_ASSERT_FALSE_MESSAGE(f.tx, msg);
    TEST_ASSERT_EQUAL_MESSAGE(MODE_USB, f.mode, msg);
    TEST_ASSERT_EQUAL_MESSAGE(CatBridgeCore::Mode::Normal, f.core.mode(), msg);
    expectPcStream(f, expect, en);
  }
}

void test_a_refused_tune_does_not_disturb_the_pc() {
  Fake f;
  f.freq = 145500000;
  uint8_t expect[400];
  uint16_t en = 0, pn = 0;
  Poll polls[200];
  // (expectations assume 14.25MHz: build a 2m stream by hand instead)
  for (uint32_t t = 20; t < 1500; t += 61) f.pcSendFrame(0, 0x03, t);
  TuneCycle c(f);
  uint32_t end = runCycle(f, c, TUNE_FULL);
  flush(f, end + 801, 1500);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Refused, outcomeOf(c, why));
  int queries = 0;
  for (uint32_t t = 20; t < 1500; t += 61) queries++;
  TEST_ASSERT_EQUAL(queries * 5, f.pcOutN); // every query answered
  uint8_t r[5];
  catEncodeFreq(145500000, r);
  for (int q = 0; q < queries; q++) {
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL(r[i], f.pcOut[q * 5 + i].b);
    TEST_ASSERT_EQUAL(MODE_USB, f.pcOut[q * 5 + 4].b);
  }
  (void)expect; (void)en; (void)pn; (void)polls;
}

void test_fifty_mhz_band() {
  Fake f;
  f.freq = 50150000;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(1, c.band());
}

void test_bench_modes() {
  { // dry run: sequencer and CAT mode only, nothing keyed, no START
    Fake f;
    TuneCycle c(f);
    runCycle(f, c, TUNE_DRY);
    TuneReason why;
    TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
    TEST_ASSERT_EQUAL(0, f.pttOnCount);
    TEST_ASSERT_EQUAL(0, f.startAt);
    expectSafe(f, MODE_USB);
  }
  { // handshake only: START/KEY runs, radio never keyed
    Fake f;
    TuneCycle c(f);
    runCycle(f, c, TUNE_ATU_NO_RF);
    TuneReason why;
    TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
    TEST_ASSERT_EQUAL(0, f.pttOnCount);
    TEST_ASSERT_GREATER_THAN(0, f.startAt);
    expectSafe(f, MODE_USB);
  }
}

// ---- refusals (nothing may be left changed) ----

void test_refuses_when_a_band_is_busy() {
  Fake f;
  f.seq.stby(2, true, 0); // 144M transmitting
  TuneCycle c(f);
  TEST_ASSERT_FALSE(c.start(TUNE_FULL_ONKEY, 1));
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Refused, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(TuneReason::BandBusy, why);
  TEST_ASSERT_FALSE(c.active());
}

void test_refuses_when_radio_is_transmitting() {
  Fake f;
  f.tx = true;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Refused, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(TuneReason::RadioTransmitting, why);
  TEST_ASSERT_EQUAL(0, f.startAt);
  TEST_ASSERT_EQUAL(0, f.amSetAt);
}

void test_refuses_vhf_and_uhf() {
  const uint32_t freqs[] = {145500000, 433500000};
  for (uint32_t hz : freqs) {
    Fake f;
    f.freq = hz;
    TuneCycle c(f);
    runCycle(f, c, TUNE_FULL_ONKEY);
    TuneReason why;
    TEST_ASSERT_EQUAL(TuneOutcome::Refused, outcomeOf(c, why));
    TEST_ASSERT_EQUAL(TuneReason::BandUnsupported, why);
    TEST_ASSERT_EQUAL(0, f.amSetAt);
    expectSafe(f, MODE_USB);
  }
}

void test_atu_flag_per_band_decides_what_may_be_tuned() {
  { // an EDX-2 owner turns 50MHz off: refused, nothing touched
    Fake f;
    f.freq = 50150000;
    f.cfg.band[1].atu = false;
    TuneCycle c(f);
    runCycle(f, c, TUNE_FULL);
    TuneReason why;
    TEST_ASSERT_EQUAL(TuneOutcome::Refused, outcomeOf(c, why));
    TEST_ASSERT_EQUAL(TuneReason::BandUnsupported, why);
    TEST_ASSERT_EQUAL(0, f.amSetAt);
    TEST_ASSERT_EQUAL(0, f.startAt);
    expectSafe(f, MODE_USB);
  }
  { // ...while HF still tunes
    Fake f;
    f.cfg.band[1].atu = false;
    TuneCycle c(f);
    runCycle(f, c, TUNE_FULL);
    TuneReason why;
    TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  }
  { // and a band can be turned on that is off by default
    Fake f;
    f.freq = 145500000;
    f.cfg.band[2].atu = true;
    TuneCycle c(f);
    runCycle(f, c, TUNE_FULL);
    TuneReason why;
    TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  }
  { // HF itself can be switched off
    Fake f;
    f.cfg.band[0].atu = false;
    TuneCycle c(f);
    runCycle(f, c, TUNE_FULL);
    TuneReason why;
    TEST_ASSERT_EQUAL(TuneOutcome::Refused, outcomeOf(c, why));
    TEST_ASSERT_EQUAL(TuneReason::BandUnsupported, why);
  }
}

void test_refuses_a_frequency_in_no_band() {
  Fake f;
  f.freq = 100000000;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Refused, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(TuneReason::NoBand, why);
  expectSafe(f, MODE_USB);
}

void test_start_rejected_while_running() {
  Fake f;
  TuneCycle c(f);
  TEST_ASSERT_TRUE(c.start(TUNE_FULL_ONKEY, 0));
  TEST_ASSERT_FALSE(c.start(TUNE_FULL_ONKEY, 1));
  TuneOutcome o;
  TuneReason r;
  TEST_ASSERT_FALSE(c.takeOutcome(o, r)); // no outcome while running
}

// ---- failures at each step ----

void test_bus_never_free() {
  Fake f;
  f.pcNeverPauses = true; // the PC polls constantly and the radio never answers it
  f.radioMute = true;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Failed, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(TuneReason::BusTimeout, why);
  expectSafe(f, MODE_USB);
}

void test_tx_status_gets_no_answer() {
  Fake f;
  f.dropF7 = true;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Failed, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(TuneReason::CatFailed, why);
  TEST_ASSERT_EQUAL(0, f.amSetAt);
  expectSafe(f, MODE_USB);
}

void test_freq_query_gets_no_answer() {
  Fake f;
  f.dropFreq = true;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Failed, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(TuneReason::CatFailed, why);
  expectSafe(f, MODE_USB);
}

void test_unreadable_mode_means_the_radio_is_not_touched() {
  Fake f;
  f.badMode = true;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Failed, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(TuneReason::CatFailed, why);
  TEST_ASSERT_EQUAL(0, f.amSetAt);
  TEST_ASSERT_EQUAL(0, f.startAt);
}

void test_sequencer_never_comes_up() {
  Fake f;
  f.seqStuck = true;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Failed, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(TuneReason::SequencerTimeout, why);
  TEST_ASSERT_EQUAL(0, f.startAt); // never started the tuner
  expectSafe(f, MODE_USB);
}

void test_no_atu() {
  Fake f;
  f.noAtu = true;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Failed, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(TuneReason::NoAtu, why);
  TEST_ASSERT_EQUAL(0, f.pttOnCount); // never keyed with nothing to tune
  expectSafe(f, MODE_USB);
}

void test_key_stuck() {
  Fake f;
  f.keyStuck = true;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Failed, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(TuneReason::KeyStuck, why);
  TEST_ASSERT_FALSE(f.ah4.startAsserted());
  expectSafe(f, MODE_USB);
}

void test_tuner_never_releases_key() {
  Fake f;
  f.neverRelease = true;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Failed, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(TuneReason::AtuTimeout, why);
  expectSafe(f, MODE_USB); // PTT must come off even though KEY never did
}

void test_tuner_reports_not_tuned() {
  Fake f;
  f.failTune = true;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Failed, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(TuneReason::TuneFailed, why);
  TEST_ASSERT_LESS_THAN(AH4_CONFIRM_MS, f.pttOffAt - f.keyReleaseAt); // unkeyed at the first release
  expectSafe(f, MODE_USB);
}

void test_ptt_on_fails() {
  Fake f;
  f.failPttOn = true;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Failed, outcomeOf(c, why));
  expectSafe(f, MODE_USB); // and PTT off was still sent in case it half-worked
}

void test_ptt_off_is_retried_until_it_works() {
  Fake f;
  f.failPttOffTimes = 3;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(4, f.pttOffCount);
  expectSafe(f, MODE_USB);
}

void test_mode_restore_is_retried_then_given_up() {
  Fake f;
  f.failRestoreTimes = 1;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TEST_ASSERT_FALSE(c.modeRestoreFailed());
  expectSafe(f, MODE_USB);

  Fake g;
  g.failRestoreTimes = 99;
  TuneCycle d(g);
  runCycle(g, d, TUNE_FULL_ONKEY);
  TEST_ASSERT_TRUE(d.modeRestoreFailed()); // reported loudly by the caller; cycle still ends
  TEST_ASSERT_FALSE(g.tx);
  TEST_ASSERT_EQUAL(CatArbiter::ClaimState::None, g.core.claimState());
}

void test_watchdog_ends_a_stuck_cycle() {
  Fake f;
  f.blockQueries = true; // the first query can never be sent
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY, -1, 120000);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Failed, outcomeOf(c, why));
  TEST_ASSERT_EQUAL(TuneReason::Watchdog, why);
  expectSafe(f, MODE_USB);
}

// ---- abort at every point ----

void test_abort_at_every_moment_leaves_things_safe() {
  // Walk an abort across the whole of a normal cycle (it runs ~2.5s).
  for (int32_t at = 1; at < 3200; at += 7) {
    Fake f;
    TuneCycle c(f);
    runCycle(f, c, TUNE_FULL_ONKEY, at);
    TuneOutcome o;
    TuneReason why;
    TEST_ASSERT_TRUE(c.takeOutcome(o, why));
    if (o != TuneOutcome::Aborted && o != TuneOutcome::Success) {
      char msg[64];
      snprintf(msg, sizeof msg, "abort at %d gave outcome %d", (int)at, (int)o);
      TEST_FAIL_MESSAGE(msg);
    }
    char msg[64];
    snprintf(msg, sizeof msg, "unsafe after abort at %d ms", (int)at);
    TEST_ASSERT_FALSE_MESSAGE(f.tx, msg);
    TEST_ASSERT_EQUAL_MESSAGE(CatArbiter::ClaimState::None, f.core.claimState(), msg);
    TEST_ASSERT_FALSE_MESSAGE(f.ah4.busy(), msg);
    TEST_ASSERT_TRUE_MESSAGE(f.seq.allIdle(), msg);
    TEST_ASSERT_FALSE_MESSAGE(f.seq.outputs().txInhibit, msg);
    TEST_ASSERT_FALSE_MESSAGE(f.hold[0] || f.hold[1] || f.hold[2] || f.hold[3], msg);
    TEST_ASSERT_EQUAL_MESSAGE(MODE_USB, f.mode, msg);
    TEST_ASSERT_EQUAL_MESSAGE(f.pttOnCount, f.pttOffCount > 0 ? f.pttOnCount : 0, msg);
  }
}

void test_abort_sweep_also_on_a_genuine_ah4() {
  for (int32_t at = 1; at < 3200; at += 11) {
    Fake f;
    f.genuine = true;
    TuneCycle c(f);
    runCycle(f, c, TUNE_FULL_ONKEY, at);
    char msg[64];
    snprintf(msg, sizeof msg, "unsafe after abort at %d ms", (int)at);
    TEST_ASSERT_FALSE_MESSAGE(f.tx, msg);
    TEST_ASSERT_EQUAL_MESSAGE(CatArbiter::ClaimState::None, f.core.claimState(), msg);
    TEST_ASSERT_FALSE_MESSAGE(f.ah4.busy(), msg);
    TEST_ASSERT_EQUAL_MESSAGE(MODE_USB, f.mode, msg);
  }
}

void test_abort_when_idle_is_harmless() {
  Fake f;
  TuneCycle c(f);
  c.abort(5);
  TEST_ASSERT_FALSE(c.active());
}

void test_second_cycle_after_a_failure_runs_clean() {
  Fake f;
  f.noAtu = true;
  TuneCycle c(f);
  runCycle(f, c, TUNE_FULL_ONKEY);
  TuneReason why;
  TEST_ASSERT_EQUAL(TuneOutcome::Failed, outcomeOf(c, why));
  f.noAtu = false;
  f.startAt = f.amSetAt = f.restoreAt = 0;
  f.prevStart = false;
  f.armed = false;
  uint32_t t0 = 20000;
  c.start(TUNE_FULL_ONKEY, t0);
  for (uint32_t t = t0 + 1; t < t0 + 20000 && c.active(); t++) {
    f.hw(t);
    c.poll(t);
    f.bus(t);
  }
  TEST_ASSERT_EQUAL(TuneOutcome::Success, outcomeOf(c, why));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_full_tune_on_an_edx2_style_tuner);
  RUN_TEST(test_full_tune_on_a_genuine_ah4_style_tuner);
  RUN_TEST(test_recovery_record_brackets_every_change_to_the_radio);
  RUN_TEST(test_refusals_before_the_radio_is_touched_never_arm_the_record);
  RUN_TEST(test_a_stuck_ptt_off_keeps_the_record_armed_until_it_works);
  RUN_TEST(test_already_in_am_is_left_alone);
  RUN_TEST(test_narrow_mode_restored_exactly);
  RUN_TEST(test_spurious_stby_on_another_band_cannot_disturb_the_tune);
  RUN_TEST(test_a_long_tune_is_allowed);
  RUN_TEST(test_meter_diagnostic_polls_without_breaking_the_unkey);
  RUN_TEST(test_meter_is_off_in_a_normal_tune);
  RUN_TEST(test_carrier_diagnostic_keys_the_radio_alone);
  RUN_TEST(test_abort_during_the_carrier_test_unkeys);
  RUN_TEST(test_prekey_keys_before_the_tuner_asks_and_unkeys_at_key_release);
  RUN_TEST(test_prekey_with_no_tuner_still_unkeys);
  RUN_TEST(test_prekey_abort_sweep);
  RUN_TEST(test_tune_mode_is_selectable_and_restored);
  RUN_TEST(test_default_is_to_key_at_start);
  RUN_TEST(test_default_path_failures_all_end_safe);
  RUN_TEST(test_pc_polling_through_a_whole_tune_sees_no_difference);
  RUN_TEST(test_replies_during_the_tune_are_immediate);
  RUN_TEST(test_pc_ptt_commands_cannot_key_or_unkey_the_radio);
  RUN_TEST(test_pc_mode_change_during_the_tune_is_applied_after_the_restore);
  RUN_TEST(test_pc_frequency_change_is_replayed_and_the_tune_still_used_the_old_one);
  RUN_TEST(test_queued_commands_replay_in_order_and_overflow_keeps_the_newest);
  RUN_TEST(test_a_query_the_bridge_cannot_answer_is_delayed_not_lost);
  RUN_TEST(test_a_command_split_across_every_phase_is_never_cut_in_half);
  RUN_TEST(test_abort_with_a_polling_pc_leaves_the_pc_stream_intact);
  RUN_TEST(test_a_refused_tune_does_not_disturb_the_pc);
  RUN_TEST(test_fifty_mhz_band);
  RUN_TEST(test_bench_modes);
  RUN_TEST(test_refuses_when_a_band_is_busy);
  RUN_TEST(test_refuses_when_radio_is_transmitting);
  RUN_TEST(test_refuses_vhf_and_uhf);
  RUN_TEST(test_atu_flag_per_band_decides_what_may_be_tuned);
  RUN_TEST(test_refuses_a_frequency_in_no_band);
  RUN_TEST(test_start_rejected_while_running);
  RUN_TEST(test_bus_never_free);
  RUN_TEST(test_tx_status_gets_no_answer);
  RUN_TEST(test_freq_query_gets_no_answer);
  RUN_TEST(test_unreadable_mode_means_the_radio_is_not_touched);
  RUN_TEST(test_sequencer_never_comes_up);
  RUN_TEST(test_no_atu);
  RUN_TEST(test_key_stuck);
  RUN_TEST(test_tuner_never_releases_key);
  RUN_TEST(test_tuner_reports_not_tuned);
  RUN_TEST(test_ptt_on_fails);
  RUN_TEST(test_ptt_off_is_retried_until_it_works);
  RUN_TEST(test_mode_restore_is_retried_then_given_up);
  RUN_TEST(test_watchdog_ends_a_stuck_cycle);
  RUN_TEST(test_abort_at_every_moment_leaves_things_safe);
  RUN_TEST(test_abort_sweep_also_on_a_genuine_ah4);
  RUN_TEST(test_abort_when_idle_is_harmless);
  RUN_TEST(test_second_cycle_after_a_failure_runs_clean);
  return UNITY_END();
}
