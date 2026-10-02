#include "cat_intercept.h"

#include <string.h>

InterceptDecision catIntercept(const CatSnapshot &snap, const uint8_t cmd[CAT_FRAME_LEN]) {
  InterceptDecision d;
  switch (cmd[CAT_FRAME_LEN - 1]) {
    case CAT_OP_GET_FREQ_MODE_MAIN:
      if (snap.haveFreqMode) {
        d.action = InterceptAction::Reply;
        memcpy(d.reply, snap.freqMode, CAT_FRAME_LEN);
        d.replyLen = CAT_FRAME_LEN;
      }
      break;
    case CAT_OP_TX_STATUS:
      if (snap.haveTx) {
        d.action = InterceptAction::Reply;
        d.reply[0] = snap.tx;
        d.replyLen = 1;
      }
      break;
    case CAT_OP_RX_STATUS:
      if (snap.haveRx) {
        d.action = InterceptAction::Reply;
        d.reply[0] = snap.rx;
        d.replyLen = 1;
      }
      break;
    case CAT_OP_PTT_ON:
    case CAT_OP_PTT_OFF:
      d.action = InterceptAction::Swallow;
      break;
    default:
      break; // Queue
  }
  return d;
}

bool CatFrameAssembler::push(uint8_t b, uint32_t now, uint8_t out[CAT_FRAME_LEN]) {
  if (len_ && (int32_t)(now - (lastAt_ + CAT_BYTE_TIMEOUT_MS)) >= 0) len_ = 0; // stale partial
  buf_[len_++] = b;
  lastAt_ = now;
  if (len_ < CAT_FRAME_LEN) return false;
  memcpy(out, buf_, CAT_FRAME_LEN);
  len_ = 0;
  return true;
}

bool CatFrameAssembler::poll(uint32_t now) {
  if (len_ && (int32_t)(now - (lastAt_ + CAT_BYTE_TIMEOUT_MS)) >= 0) {
    len_ = 0;
    return true;
  }
  return false;
}

bool CatReplayQueue::push(const uint8_t cmd[CAT_FRAME_LEN]) {
  bool dropped = false;
  if (count_ == CAT_REPLAY_MAX) { // drop the oldest
    head_ = (head_ + 1) % CAT_REPLAY_MAX;
    count_--;
    dropped_++;
    dropped = true;
  }
  memcpy(buf_[(head_ + count_) % CAT_REPLAY_MAX], cmd, CAT_FRAME_LEN);
  count_++;
  return dropped;
}

bool CatReplayQueue::pop(uint8_t out[CAT_FRAME_LEN]) {
  if (!count_) return false;
  memcpy(out, buf_[head_], CAT_FRAME_LEN);
  head_ = (head_ + 1) % CAT_REPLAY_MAX;
  count_--;
  return true;
}
