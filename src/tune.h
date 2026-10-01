#pragma once

#include <stdint.h>

#include "ah4.h"
#include "cat_arbiter.h"
#include "cat_codec.h"
#include "sequencer.h"

// The ATU tune cycle as a pure state machine: no pins, no Serial, time passed in,
// all hardware behind TuneEnv so it can be driven by fakes in host tests. See
// README "ATU tune cycle". Every path - success, refusal, failure, abort - ends
// in the same tail that leaves things safe: PTT off, original mode restored, CAT
// bus released, sequencer stepped back down, the band's STBY hold dropped.
//
// Order (head):  claim CAT bus -> CAT on -> TX status -> freq/mode -> tune-profile
//   up-sequence (band held) -> AM mode -> START/KEY handshake, keying PTT at START
//   (or the instant KEY asserts, with TUNE_FULL_ONKEY) and unkeying at KEY's first
//   release.
// Order (tail):  PTT off -> restore mode -> release bus -> sequencer down -> done.

class TuneEnv {
public:
  // CAT on Port 2, via the arbiter's claim
  virtual bool catClaim(uint32_t now) = 0;
  virtual CatArbiter::ClaimState catClaimState() = 0;
  virtual void catRelease(uint32_t now) = 0;
  virtual bool catSubmit(const uint8_t cmd[5], uint32_t now) = 0; // false: busy, retry
  virtual bool catTakeResult(CatArbiter::Result &r, uint8_t *reply, uint8_t &len) = 0;
  // Sequencer
  virtual bool bandsAllIdle() = 0;
  virtual void seqHold(uint8_t band, bool held) = 0;
  virtual void seqRequest(uint8_t band, bool wantTx, uint32_t now) = 0; // tune profile
  virtual bool seqActive(uint8_t band) = 0;
  virtual bool seqIdle(uint8_t band) = 0;
  virtual int8_t bandForFreq(uint32_t hz) = 0;
  virtual bool atuAllowed(uint8_t band) = 0; // the config's per-band "atu" flag
  // AH-4
  virtual bool ah4Begin(uint32_t now) = 0;
  virtual bool ah4KeySeen() = 0;
  virtual bool ah4KeyReleased() = 0;
  virtual Ah4Driver::Result ah4TakeResult() = 0;
  virtual void ah4Abort(uint32_t now) = 0;
  virtual bool ah4Busy() = 0;

protected:
  ~TuneEnv() {}
};

constexpr uint32_t TUNE_WATCHDOG_MS = 45000;  // whole head, from start(): up-sequence + 15s tune + margin
constexpr uint32_t TUNE_SEQ_TIMEOUT_MS = 16000; // 3 stages x 5000ms max + margin
constexpr uint32_t TUNE_DRY_DWELL_MS = 1000;
constexpr uint32_t TUNE_CARRIER_MS = 2000;
constexpr uint8_t TUNE_MODE_RESTORE_TRIES = 3;

struct TuneOptions {
  bool ah4;   // run the START/KEY handshake
  bool key;   // key the radio when KEY asserts (needs ah4)
  bool meter; // diagnostic: poll the radio's PO meter while keyed (can delay PTT off by up to ~100ms)
  bool carrier; // diagnostic: no ATU; key the radio for TUNE_CARRIER_MS
  bool preKey;  // key the radio when START asserts rather than when KEY does, so the
                // radio's start-up power overshoot has settled before the tuner measures
  uint8_t mode; // radio mode to tune in (MODE_AM by default)
};
// The default keys the radio at START: bench-proven on an Alinco EDX-2, where keying
// at KEY made the tuner measure the radio's ~0.7s start-up overshoot and give up.
constexpr TuneOptions TUNE_FULL = {true, true, false, false, true, MODE_AM};
constexpr TuneOptions TUNE_FULL_PREKEY = TUNE_FULL;
constexpr TuneOptions TUNE_FULL_ONKEY = {true, true, false, false, false, MODE_AM}; // key when KEY asserts
constexpr TuneOptions TUNE_FULL_METER = {true, true, true, false, true, MODE_AM};
constexpr TuneOptions TUNE_ATU_NO_RF = {true, false, false, false, false, MODE_AM}; // no carrier
constexpr TuneOptions TUNE_DRY = {false, false, false, false, false, MODE_AM};      // sequencer + CAT mode
constexpr TuneOptions TUNE_CARRIER = {false, false, true, true, false, MODE_AM};    // carrier + meter

enum class TuneOutcome : uint8_t { None, Success, Refused, Failed, Aborted };

enum class TuneReason : uint8_t {
  None,
  BandBusy,          // a band is already sequencing / transmitting
  RadioTransmitting, // radio reports it is transmitting
  BusTimeout,        // could not claim the CAT bus
  CatFailed,         // a CAT query got no/invalid answer
  NoBand,            // frequency is not in any configured band
  BandUnsupported,   // the ATU is not enabled for this band in the config
  SequencerTimeout,
  Ah4Busy,           // START still held from a previous cycle
  KeyStuck,          // KEY already asserted before START
  NoAtu,             // KEY never asserted
  AtuTimeout,        // KEY never released
  TuneFailed,        // the AH-4's not-tuned signal
  RfNotKeyed,        // tuner finished but the radio was never keyed
  Watchdog,
  Aborted,
};

enum class TuneStep : uint8_t {
  Idle,
  Claim, CatOn, QueryTx, QueryFreq, SeqUp, SetAm, Ah4, Dwell, // head
  TailPtt, TailMode, TailSeq,                                  // tail
};

class TuneCycle {
public:
  explicit TuneCycle(TuneEnv &env) : env_(env) {}

  // Begin a cycle. False if one is already running, or it was refused up front
  // (the refusal is then available from takeOutcome).
  bool start(TuneOptions options, uint32_t now);

  // Stop early: goes straight to the tail. Ignored once the tail has begun.
  void abort(uint32_t now);

  void poll(uint32_t now);

  bool active() const { return step_ != TuneStep::Idle; }
  TuneStep step() const { return step_; }

  // The cycle's outcome, once, after it has fully unwound (step() == Idle).
  bool takeOutcome(TuneOutcome &o, TuneReason &r);

  bool modeRestoreFailed() const { return modeGaveUp_; }

  // Diagnostic PO meter samples (TuneOptions::meter): once per sample, with the
  // raw 0xF7 status byte and how long after the PTT command it was read.
  bool takeMeter(uint32_t &msAfterPtt, uint8_t &status);
  int8_t band() const { return band_; }
  uint32_t freqHz() const { return freqHz_; }
  uint8_t originalMode() const { return origMode_; }

private:
  enum class Kind : uint8_t { None, CatOn, QueryTx, QueryFreq, SetAm, PttOn, PttOff, RestoreMode, Meter };
  enum class OpState : uint8_t { Idle, Pending, Ok, Failed };

  void setStep(TuneStep s, uint32_t now);
  void enterTail(TuneOutcome o, TuneReason r, uint32_t now);
  void pollOp();
  void onOpDone(Kind k, bool ok, uint32_t now);
  void trySubmit(uint32_t now);
  void pollHead(uint32_t now);
  void pollTail(uint32_t now);
  void pollAh4(uint32_t now);
  void startTuner(uint32_t now);
  void pollMeter();
  bool inTail() const { return step_ >= TuneStep::TailPtt; }

  TuneEnv &env_;
  TuneOptions options_ = TUNE_FULL;
  TuneStep step_ = TuneStep::Idle;
  uint32_t startedAt_ = 0;
  uint32_t stepAt_ = 0;

  // one CAT operation in flight, one queued behind it at most
  Kind want_ = Kind::None;
  Kind kind_ = Kind::None;
  OpState op_ = OpState::Idle;
  uint8_t reply_[CAT_FRAME_LEN] = {};
  uint8_t replyLen_ = 0;

  int8_t band_ = -1;
  uint32_t freqHz_ = 0;
  uint8_t origMode_ = 0;
  bool modeChanged_ = false;
  bool modeRestored_ = false;
  bool modeGaveUp_ = false;
  uint8_t restoreTries_ = 0;

  bool holdSet_ = false; // every band's STBY handling is held
  bool seqRequested_ = false;
  bool ah4Started_ = false;
  bool pttRequested_ = false; // KEY cue seen, PTT on wanted
  bool pttOnSent_ = false;
  bool pttOffWanted_ = false;
  bool pttOffDone_ = false;
  uint32_t pttOnAt_ = 0;
  bool meterNew_ = false;
  uint8_t meterStatus_ = 0;
  uint32_t meterAt_ = 0;

  TuneOutcome pendingOutcome_ = TuneOutcome::None;
  TuneReason pendingReason_ = TuneReason::None;
  TuneOutcome outcome_ = TuneOutcome::None;
  TuneReason reason_ = TuneReason::None;
};
