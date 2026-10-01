#include "cat_arbiter.h"

#include <string.h>

static bool reached(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

bool CatArbiter::submit(const uint8_t cmd[CAT_FRAME_LEN], uint32_t now) {
  if (state_ != State::Idle) return false;
  memcpy(cmd_, cmd, CAT_FRAME_LEN);
  replyWanted_ = catReplyLength(cmd[CAT_FRAME_LEN - 1]);
  replyGot_ = 0;
  submittedAt_ = now;
  gateClosed_ = false;
  result_ = Result::None;
  state_ = State::WaitBus;
  return true;
}

bool CatArbiter::pcMayTransmit() const {
  switch (state_) {
    case State::Idle: return true;
    case State::WaitBus: return !gateClosed_;
    default: return false;
  }
}

void CatArbiter::finish(Result r, uint32_t now) {
  result_ = r;
  gateClosed_ = false;
  if (r == Result::BusTimeout) {
    state_ = State::Idle; // never touched the bus, nothing to hold off
  } else {
    state_ = State::Hold;
    deadline_ = now + ARB_QUIET_MS;
  }
}

void CatArbiter::poll(uint32_t now, bool pcBusIdle) {
  switch (state_) {
    case State::WaitBus:
      if ((uint32_t)(now - submittedAt_) >= ARB_BUS_TIMEOUT_MS) {
        finish(Result::BusTimeout, now);
        break;
      }
      if (!gateClosed_ && pcBusIdle) gateClosed_ = true; // PC held from here on
      if (gateClosed_ && (uint32_t)(now - lastActivity_) >= ARB_QUIET_MS) {
        state_ = State::Sending;
      }
      break;
    case State::WaitReply:
      if (reached(now, deadline_)) finish(Result::NoReply, now);
      break;
    case State::Hold:
      if (reached(now, deadline_)) state_ = State::Idle;
      break;
    default:
      break;
  }
}

void CatArbiter::sent(uint32_t now) {
  if (state_ != State::Sending) return;
  if (replyWanted_) {
    state_ = State::WaitReply;
    deadline_ = now + ARB_REPLY_TIMEOUT_MS;
  } else {
    finish(Result::Ok, now);
  }
}

void CatArbiter::radioByte(uint8_t b, uint32_t now) {
  if (state_ != State::WaitReply) return;
  reply_[replyGot_++] = b;
  if (replyGot_ >= replyWanted_) finish(Result::Ok, now);
}

bool CatArbiter::takeResult(Result &r, uint8_t *reply, uint8_t &len) {
  if (result_ == Result::None) return false;
  r = result_;
  len = r == Result::Ok ? replyWanted_ : replyGot_;
  memcpy(reply, reply_, len);
  result_ = Result::None;
  return true;
}
