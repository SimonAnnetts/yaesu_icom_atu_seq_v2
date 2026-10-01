#include "ah4.h"

static bool reached(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

bool Ah4Driver::begin(bool keyAsserted, uint32_t now) {
  if (busy()) return false;
  keySeen_ = false;
  if (keyAsserted) {
    result_ = Result::KeyStuck; // never drive START into a tuner that already says busy
    return true;
  }
  result_ = Result::None;
  startOn_ = true;
  startedAt_ = now;
  startReleaseAt_ = now + AH4_START_HOLD_MS;
  keyDeadline_ = startReleaseAt_ + AH4_KEY_APPEAR_TIMEOUT_MS;
  state_ = State::WaitKey;
  return true;
}

// Bring START's release forward to the earliest allowed moment.
void Ah4Driver::shortenStartHold(uint32_t now) {
  if (!startOn_) return;
  uint32_t earliest = (uint32_t)(now - startedAt_) >= AH4_START_MIN_HOLD_MS
                          ? now
                          : startedAt_ + AH4_START_MIN_HOLD_MS;
  if (reached(startReleaseAt_, earliest)) startReleaseAt_ = earliest; // only ever earlier
}

void Ah4Driver::finish(Result r, uint32_t now, bool releaseStartEarly) {
  result_ = r;
  state_ = State::Idle;
  if (releaseStartEarly) shortenStartHold(now);
  if (startOn_ && reached(now, startReleaseAt_)) startOn_ = false;
}

void Ah4Driver::poll(bool keyAsserted, uint32_t now) {
  if (startOn_ && reached(now, startReleaseAt_)) startOn_ = false;

  switch (state_) {
    case State::WaitKey:
      if (keyAsserted) {
        keySeen_ = true;
        state_ = State::Busy;
        deadline_ = now + AH4_BUSY_TIMEOUT_MS;
      } else if (reached(now, keyDeadline_)) {
        finish(Result::NoAtu, now, true);
      }
      break;
    case State::Busy:
      if (!keyAsserted) {
        state_ = State::Confirm;
        deadline_ = now + AH4_CONFIRM_MS;
      } else if (reached(now, deadline_)) {
        finish(Result::Timeout, now, true);
      }
      break;
    case State::Confirm:
      if (keyAsserted) {
        finish(Result::TuneFailed, now, true);
      } else if (reached(now, deadline_)) {
        finish(Result::Success, now, false); // START keeps its normal hold
      }
      break;
    default:
      break;
  }
}

void Ah4Driver::abort(uint32_t now) {
  if (state_ != State::Idle) {
    finish(Result::Aborted, now, true);
  } else {
    shortenStartHold(now); // cycle over but START still held: stop holding it
    if (startOn_ && reached(now, startReleaseAt_)) startOn_ = false;
  }
}

Ah4Driver::Result Ah4Driver::takeResult() {
  if (state_ != State::Idle) return Result::None; // not decided yet
  Result r = result_;
  result_ = Result::None;
  return r;
}
