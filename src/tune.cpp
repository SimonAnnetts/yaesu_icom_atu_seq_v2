#include "tune.h"

#include "cat_codec.h"

static bool reached(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

void TuneCycle::setStep(TuneStep s, uint32_t now) {
  step_ = s;
  stepAt_ = now;
}

bool TuneCycle::start(TuneOptions options, uint32_t now) {
  if (active()) return false;
  options_ = options;
  if (!options_.ah4) options_.key = false; // nothing to key against without the tuner
  outcome_ = TuneOutcome::None;
  reason_ = TuneReason::None;
  want_ = kind_ = Kind::None;
  op_ = OpState::Idle;
  band_ = -1;
  freqHz_ = 0;
  origMode_ = 0;
  modeChanged_ = modeRestored_ = modeGaveUp_ = false;
  restoreTries_ = 0;
  holdSet_ = seqRequested_ = ah4Started_ = false;
  pttRequested_ = pttOnSent_ = pttOffWanted_ = pttOffDone_ = false;
  meterNew_ = false;
  startedAt_ = now;

  if (!env_.bandsAllIdle()) {
    outcome_ = TuneOutcome::Refused;
    reason_ = TuneReason::BandBusy;
    return false;
  }
  if (!env_.catClaim(now)) {
    outcome_ = TuneOutcome::Refused;
    reason_ = TuneReason::BusTimeout;
    return false;
  }
  setStep(TuneStep::Claim, now);
  return true;
}

void TuneCycle::abort(uint32_t now) {
  if (active() && !inTail()) enterTail(TuneOutcome::Aborted, TuneReason::Aborted, now);
}

void TuneCycle::enterTail(TuneOutcome o, TuneReason r, uint32_t now) {
  if (inTail()) return;
  pendingOutcome_ = o;
  pendingReason_ = r;
  // The driver already knows how a finished cycle went; stop it only if it is
  // still mid-flight (START keeps its normal hold after a success).
  if (ah4Started_ && o != TuneOutcome::Success) env_.ah4Abort(now);
  if (want_ == Kind::PttOn) want_ = Kind::None; // never sent: nothing to undo
  else if (want_ != Kind::None && want_ != Kind::PttOff) want_ = Kind::None; // incl. a queued meter query
  setStep(TuneStep::TailPtt, now);
}

bool TuneCycle::takeMeter(uint32_t &msAfterPtt, uint8_t &status) {
  if (!meterNew_) return false;
  meterNew_ = false;
  msAfterPtt = meterAt_ - pttOnAt_;
  status = meterStatus_;
  return true;
}

bool TuneCycle::takeOutcome(TuneOutcome &o, TuneReason &r) {
  if (active() || outcome_ == TuneOutcome::None) return false;
  o = outcome_;
  r = reason_;
  outcome_ = TuneOutcome::None;
  return true;
}

// ---- CAT operations: at most one in flight, one wanted behind it ----


void TuneCycle::trySubmit(uint32_t now) {
  if (want_ == Kind::None || op_ != OpState::Idle) return;
  uint8_t cmd[CAT_FRAME_LEN];
  switch (want_) {
    case Kind::CatOn: catCmdCatOn(cmd); break;
    case Kind::QueryTx:
    case Kind::Meter: catCmdGetTxStatus(cmd); break;
    case Kind::QueryFreq: catCmdGetFreqMode(cmd); break;
    case Kind::SetAm: catCmdSetMode(cmd, options_.mode); break; // the tune mode
    case Kind::PttOn: catCmdPtt(cmd, true); break;
    case Kind::PttOff: catCmdPtt(cmd, false); break;
    default: catCmdSetMode(cmd, origMode_); break; // RestoreMode
  }
  if (!env_.catSubmit(cmd, now)) return; // arbiter not ready yet: retry next poll
  kind_ = want_;
  want_ = Kind::None;
  op_ = OpState::Pending;
  if (kind_ == Kind::PttOn) {
    pttOnSent_ = true; // from here the radio may be transmitting
    pttOnAt_ = now;
  }
}

void TuneCycle::pollOp() {
  if (op_ != OpState::Pending) return;
  CatArbiter::Result r;
  uint8_t len = 0;
  if (!env_.catTakeResult(r, reply_, len)) return;
  replyLen_ = len;
  op_ = r == CatArbiter::Result::Ok ? OpState::Ok : OpState::Failed;
}

void TuneCycle::onOpDone(Kind k, bool ok, uint32_t now) {
  switch (k) {
    case Kind::CatOn:
      if (step_ != TuneStep::CatOn) break;
      if (!ok) { enterTail(TuneOutcome::Failed, TuneReason::CatFailed, now); break; }
      setStep(TuneStep::QueryTx, now);
      want_ = Kind::QueryTx;
      break;
    case Kind::QueryTx:
      if (step_ != TuneStep::QueryTx) break;
      if (!ok || replyLen_ < 1) { enterTail(TuneOutcome::Failed, TuneReason::CatFailed, now); break; }
      if (catTxStatusTransmitting(reply_[0])) {
        enterTail(TuneOutcome::Refused, TuneReason::RadioTransmitting, now);
        break;
      }
      setStep(TuneStep::QueryFreq, now);
      want_ = Kind::QueryFreq;
      break;
    case Kind::QueryFreq:
      if (step_ != TuneStep::QueryFreq) break;
      if (!ok || replyLen_ < 5 || !catDecodeFreq(reply_, freqHz_) || !catModeValid(reply_[4])) {
        // An unreadable mode can't be restored afterwards, so don't touch the radio.
        enterTail(TuneOutcome::Failed, TuneReason::CatFailed, now);
        break;
      }
      origMode_ = reply_[4];
      band_ = env_.bandForFreq(freqHz_);
      if (band_ < 0) { enterTail(TuneOutcome::Refused, TuneReason::NoBand, now); break; }
      if (!tuneBandSupported((uint8_t)band_)) {
        enterTail(TuneOutcome::Refused, TuneReason::BandUnsupported, now);
        break;
      }
      // STBY must not run the normal profile on the tuned band, and only that band
      // can legitimately be transmitting: any STBY on another band during the
      // tune is spurious (RF pickup), and its TX INHIBIT glitches could chop the
      // carrier the tuner is measuring. So hold them all.
      for (uint8_t b = 0; b < SEQ_BANDS; b++) env_.seqHold(b, true);
      holdSet_ = true;
      env_.seqRequest((uint8_t)band_, true, now);
      seqRequested_ = true;
      setStep(TuneStep::SeqUp, now);
      break;
    case Kind::SetAm:
      if (step_ != TuneStep::SetAm) break;
      if (!ok) { enterTail(TuneOutcome::Failed, TuneReason::CatFailed, now); break; }
      startTuner(now);
      break;
    case Kind::Meter:
      if (ok && replyLen_ >= 1) {
        meterStatus_ = reply_[0];
        meterAt_ = now;
        meterNew_ = true;
      }
      break;
    case Kind::PttOn:
      if (!ok) enterTail(TuneOutcome::Failed, TuneReason::CatFailed, now);
      break;
    case Kind::PttOff:
      if (ok) pttOffDone_ = true;
      else pttOffWanted_ = true; // try again
      break;
    case Kind::RestoreMode:
      if (ok) {
        modeRestored_ = true;
      } else if (++restoreTries_ >= TUNE_MODE_RESTORE_TRIES) {
        modeGaveUp_ = true; // reported loudly by the caller
      }
      break;
    default:
      break;
  }
}

// ---- head ----

// After the mode is right: start the START/KEY handshake, or just dwell (dry run).
void TuneCycle::startTuner(uint32_t now) {
  if (!options_.ah4) {
    setStep(TuneStep::Dwell, now);
    if (options_.carrier) want_ = Kind::PttOn; // carrier-only diagnostic: no tuner to wait for
    return;
  }
  if (!env_.ah4Begin(now)) {
    enterTail(TuneOutcome::Failed, TuneReason::Ah4Busy, now);
    return;
  }
  ah4Started_ = true;
  setStep(TuneStep::Ah4, now);
  if (options_.preKey && options_.key) { // let the radio's start-up overshoot settle first
    pttRequested_ = true;
    want_ = Kind::PttOn;
  }
}

// Diagnostic: while keyed, keep asking the radio for its PO meter. Never ahead of
// a PTT-off, which always takes the next free slot.
void TuneCycle::pollMeter() {
  if (options_.meter && pttOnSent_ && !pttOffWanted_ && !pttOffDone_ && want_ == Kind::None &&
      op_ == OpState::Idle) {
    want_ = Kind::Meter;
  }
}

void TuneCycle::pollAh4(uint32_t now) {
  // The cue: the instant KEY asserts, key the radio.
  if (options_.key && !pttRequested_ && env_.ah4KeySeen()) {
    pttRequested_ = true;
    want_ = Kind::PttOn;
  }
  // KEY's first release: unkey at once, before the confirm window decides the result.
  if (pttRequested_ && !pttOffWanted_ && env_.ah4KeyReleased()) {
    pttOffWanted_ = true;
    if (want_ == Kind::PttOn) want_ = Kind::None; // never went out: nothing to undo
  }
  if (pttOffWanted_ && pttOnSent_ && !pttOffDone_ && want_ == Kind::None &&
      !(op_ == OpState::Pending && kind_ == Kind::PttOff)) {
    want_ = Kind::PttOff;
  }

  pollMeter();

  Ah4Driver::Result r = env_.ah4TakeResult();
  switch (r) {
    case Ah4Driver::Result::None: return;
    case Ah4Driver::Result::Success:
      if (options_.key && !pttOnSent_) {
        enterTail(TuneOutcome::Failed, TuneReason::RfNotKeyed, now);
      } else {
        enterTail(TuneOutcome::Success, TuneReason::None, now);
      }
      return;
    case Ah4Driver::Result::NoAtu: enterTail(TuneOutcome::Failed, TuneReason::NoAtu, now); return;
    case Ah4Driver::Result::Timeout: enterTail(TuneOutcome::Failed, TuneReason::AtuTimeout, now); return;
    case Ah4Driver::Result::TuneFailed: enterTail(TuneOutcome::Failed, TuneReason::TuneFailed, now); return;
    case Ah4Driver::Result::KeyStuck: enterTail(TuneOutcome::Failed, TuneReason::KeyStuck, now); return;
    case Ah4Driver::Result::Aborted: enterTail(TuneOutcome::Aborted, TuneReason::Aborted, now); return;
  }
}

void TuneCycle::pollHead(uint32_t now) {
  if (reached(now, startedAt_ + TUNE_WATCHDOG_MS)) {
    enterTail(TuneOutcome::Failed, TuneReason::Watchdog, now);
    return;
  }
  switch (step_) {
    case TuneStep::Claim: {
      CatArbiter::ClaimState cs = env_.catClaimState();
      if (cs == CatArbiter::ClaimState::Held) {
        setStep(TuneStep::CatOn, now);
        want_ = Kind::CatOn;
      } else if (cs == CatArbiter::ClaimState::Failed) {
        enterTail(TuneOutcome::Failed, TuneReason::BusTimeout, now);
      }
      break;
    }
    case TuneStep::SeqUp:
      if (env_.seqActive((uint8_t)band_)) {
        setStep(TuneStep::SetAm, now);
        if (origMode_ != options_.mode) {
          modeChanged_ = true;
          want_ = Kind::SetAm;
        } else {
          startTuner(now); // already AM: nothing to change
        }
      } else if (reached(now, stepAt_ + TUNE_SEQ_TIMEOUT_MS)) {
        enterTail(TuneOutcome::Failed, TuneReason::SequencerTimeout, now);
      }
      break;
    case TuneStep::Ah4:
      pollAh4(now);
      break;
    case TuneStep::Dwell:
      pollMeter();
      if (reached(now, stepAt_ + (options_.carrier ? TUNE_CARRIER_MS : TUNE_DRY_DWELL_MS))) {
        enterTail(TuneOutcome::Success, TuneReason::None, now);
      }
      break;
    default:
      break; // CatOn / QueryTx / QueryFreq / SetAm advance on operation results
  }
}

// ---- tail ----

void TuneCycle::pollTail(uint32_t now) {
  switch (step_) {
    case TuneStep::TailPtt:
      if (pttOnSent_ && !pttOffDone_) {
        if (want_ == Kind::None && !(op_ == OpState::Pending && kind_ == Kind::PttOff)) {
          want_ = Kind::PttOff; // keep asking until the radio has been told
        }
      } else {
        setStep(TuneStep::TailMode, now);
      }
      break;
    case TuneStep::TailMode:
      if (modeChanged_ && !modeRestored_ && !modeGaveUp_) {
        if (want_ == Kind::None && !(op_ == OpState::Pending && kind_ == Kind::RestoreMode)) {
          want_ = Kind::RestoreMode;
        }
      } else {
        env_.catRelease(now); // PC traffic can flow again while the sequencer winds down
        if (seqRequested_) env_.seqRequest((uint8_t)band_, false, now);
        setStep(TuneStep::TailSeq, now);
      }
      break;
    case TuneStep::TailSeq:
      if ((!seqRequested_ || env_.seqIdle((uint8_t)band_)) && !env_.ah4Busy()) {
        if (holdSet_) {
          for (uint8_t b = 0; b < SEQ_BANDS; b++) env_.seqHold(b, false);
        }
        outcome_ = pendingOutcome_;
        reason_ = pendingReason_;
        setStep(TuneStep::Idle, now);
      }
      break;
    default:
      break;
  }
}

void TuneCycle::poll(uint32_t now) {
  if (!active()) return;
  pollOp();
  if (op_ == OpState::Ok || op_ == OpState::Failed) {
    bool ok = op_ == OpState::Ok;
    op_ = OpState::Idle;
    onOpDone(kind_, ok, now);
  }
  if (inTail()) pollTail(now);
  else pollHead(now);
  if (active()) trySubmit(now); // a request made above goes out in the same poll
}
