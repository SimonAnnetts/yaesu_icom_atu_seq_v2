#include "cat_frame.h"

#include <string.h>

static bool reached(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

uint8_t catReplyLength(uint8_t opcode) {
  switch (opcode) {
    case CAT_OP_RX_STATUS:
    case CAT_OP_TX_STATUS:
      return 1;
    case 0x03: // main
    case 0x13: // sat RX
    case 0x23: // sat TX
      return 5;
    default:
      return 0;
  }
}

CatEvent CatFramer::pcByte(uint8_t b, uint32_t now) {
  CatEvent ev;
  // A stalled partial block is dropped here too, in case poll() hasn't run.
  if (cmdLen_ && reached(now, lastCmdByteAt_ + CAT_BYTE_TIMEOUT_MS)) cmdLen_ = 0;

  cmd_[cmdLen_++] = b;
  lastCmdByteAt_ = now;
  if (cmdLen_ < CAT_FRAME_LEN) return ev;

  cmdLen_ = 0;
  ev.kind = CatEvent::Command;
  ev.len = CAT_FRAME_LEN;
  memcpy(ev.data, cmd_, CAT_FRAME_LEN);
  ev.opcode = cmd_[CAT_FRAME_LEN - 1];

  // A new request supersedes an unfinished reply to an older one.
  pendingLen_ = catReplyLength(ev.opcode);
  if (pendingLen_) {
    pendingOpcode_ = ev.opcode;
    replyGot_ = 0;
    replyDeadline_ = now + CAT_REPLY_TIMEOUT_MS;
  }
  return ev;
}

CatEvent CatFramer::radioByte(uint8_t b, uint32_t now) {
  (void)now;
  CatEvent ev;
  if (!pendingLen_) {
    ev.kind = CatEvent::StrayReply;
    ev.len = 1;
    ev.data[0] = b;
    return ev;
  }
  reply_[replyGot_++] = b;
  if (replyGot_ < pendingLen_) return ev;

  ev.kind = CatEvent::Reply;
  ev.opcode = pendingOpcode_;
  ev.len = pendingLen_;
  memcpy(ev.data, reply_, pendingLen_);
  pendingLen_ = 0;
  return ev;
}

CatEvent CatFramer::poll(uint32_t now) {
  CatEvent ev;
  if (cmdLen_ && reached(now, lastCmdByteAt_ + CAT_BYTE_TIMEOUT_MS)) {
    ev.kind = CatEvent::CommandAbandoned;
    ev.len = cmdLen_;
    memcpy(ev.data, cmd_, cmdLen_);
    cmdLen_ = 0;
    return ev;
  }
  if (pendingLen_ && reached(now, replyDeadline_)) {
    ev.kind = CatEvent::ReplyAbandoned;
    ev.opcode = pendingOpcode_;
    ev.len = replyGot_;
    memcpy(ev.data, reply_, replyGot_);
    pendingLen_ = 0;
    return ev;
  }
  return ev;
}
