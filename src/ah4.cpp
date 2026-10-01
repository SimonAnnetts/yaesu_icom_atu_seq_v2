#include "ah4.h"

static bool reached(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

bool Ah4Driver::begin(bool keyAsserted, uint32_t now) {
  if (state_ != State::Idle) return false;
  if (keyAsserted) {
    result_ = Result::KeyStuck; // never drive START into a tuner that already says busy
    return true;
  }
  result_ = Result::None;
  startOn_ = true;
  startedAt_ = now;
  state_ = State::WaitKey;
  return true;
}

void Ah4Driver::scheduleRelease(uint32_t now) {
  uint32_t heldFor = now - startedAt_;
  releaseAt_ = heldFor >= AH4_START_MIN_HOLD_MS ? now : startedAt_ + AH4_START_MIN_HOLD_MS;
  state_ = State::Releasing;
}

void Ah4Driver::tryRelease(uint32_t now) {
  if (state_ == State::Releasing && reached(now, releaseAt_)) {
    startOn_ = false;
    state_ = State::Idle;
  }
}

void Ah4Driver::finish(Result r, uint32_t now) {
  result_ = r;
  scheduleRelease(now);
  tryRelease(now);
}

void Ah4Driver::poll(bool keyAsserted, uint32_t now) {
  switch (state_) {
    case State::WaitKey:
      if (keyAsserted) {
        state_ = State::Busy;
        deadline_ = now + AH4_BUSY_TIMEOUT_MS;
      } else if ((uint32_t)(now - startedAt_) >= AH4_KEY_APPEAR_TIMEOUT_MS) {
        finish(Result::NoAtu, now);
      }
      break;
    case State::Busy:
      if (!keyAsserted) {
        state_ = State::Confirm;
        deadline_ = now + AH4_CONFIRM_MS;
      } else if (reached(now, deadline_)) {
        finish(Result::Timeout, now);
      }
      break;
    case State::Confirm:
      if (keyAsserted) {
        finish(Result::Bounce, now);
      } else if (reached(now, deadline_)) {
        result_ = Result::Success; // START stays asserted until release()
        state_ = State::Holding;
      }
      break;
    case State::Releasing:
      tryRelease(now);
      break;
    default:
      break;
  }
}

void Ah4Driver::release(uint32_t now) {
  if (state_ != State::Holding) return;
  scheduleRelease(now);
  tryRelease(now);
}

void Ah4Driver::abort(uint32_t now) {
  switch (state_) {
    case State::WaitKey:
    case State::Busy:
    case State::Confirm:
      finish(Result::Aborted, now);
      break;
    case State::Holding:
      release(now);
      break;
    default:
      break;
  }
}

Ah4Driver::Result Ah4Driver::takeResult() {
  Result r = result_;
  // A result that is still pending in WaitKey/Busy/Confirm isn't final yet.
  if (state_ == State::WaitKey || state_ == State::Busy || state_ == State::Confirm) return Result::None;
  result_ = Result::None;
  return r;
}
