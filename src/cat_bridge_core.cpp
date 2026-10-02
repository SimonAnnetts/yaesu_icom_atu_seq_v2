#include "cat_bridge_core.h"

using Frame = CatBridgeIo::Frame;

void CatBridgeCore::setSnapshot(const CatSnapshot &s) {
  snapshot_ = s;
  snapshotReady_ = true;
}

void CatBridgeCore::handleEvent(const CatEvent &ev) {
  switch (ev.kind) {
    case CatEvent::Command: io_.frame(Frame::PcToRadio, ev.data, ev.len); break;
    case CatEvent::Reply: io_.frame(Frame::RadioToPc, ev.data, ev.len); break;
    case CatEvent::StrayReply: io_.frame(Frame::StrayFromRadio, ev.data, ev.len); break;
    case CatEvent::CommandAbandoned: io_.frame(Frame::TornFromPc, ev.data, ev.len); break;
    case CatEvent::ReplyAbandoned: io_.frame(Frame::ReplyTimeout, ev.data, ev.len); break;
    default: break;
  }
}

void CatBridgeCore::updateMode() {
  bool held = arbiter_.claimState() == CatArbiter::ClaimState::Held;
  switch (mode_) {
    case Mode::Normal:
      if (held) {
        mode_ = Mode::Hold;
        snapshotReady_ = false; // any earlier snapshot is stale
      }
      break;
    case Mode::Hold:
      if (!held) {
        // The claim ended before there was anything to answer from (a refused or
        // failed cycle). Anything already queued still has to go out in order.
        mode_ = (queue_.count() || !asm_.idle()) ? Mode::Drain : Mode::Normal;
        snapshotReady_ = false;
      } else if (snapshotReady_) {
        mode_ = Mode::Intercept;
        asm_ = CatFrameAssembler();
      }
      break;
    case Mode::Intercept:
      if (!held) {
        mode_ = Mode::Drain;
        snapshotReady_ = false;
      }
      break;
    case Mode::Drain:
      if (held) {
        mode_ = Mode::Hold; // another tune began before the queue emptied
        snapshotReady_ = false;
      } else if (queue_.count() == 0 && asm_.idle()) {
        mode_ = Mode::Normal;
      }
      break;
  }
}

void CatBridgeCore::enqueue(const uint8_t frame[CAT_FRAME_LEN]) {
  if (queue_.push(frame)) io_.frame(Frame::DroppedFromQueue, frame, CAT_FRAME_LEN);
  io_.frame(Frame::QueuedFromPc, frame, CAT_FRAME_LEN);
}

void CatBridgeCore::interceptByte(uint8_t b, uint32_t now) {
  uint8_t frame[CAT_FRAME_LEN];
  if (!asm_.push(b, now, frame)) return;
  if (mode_ == Mode::Drain) { // the radio is real again, but order must hold
    if (queue_.count() == 0 && busReadyForReplay(now)) {
      sendToRadio(frame, now); // nothing ahead of it: straight through
    } else {
      enqueue(frame);
    }
    return;
  }
  InterceptDecision d = catIntercept(snapshot_, frame);
  switch (d.action) {
    case InterceptAction::Reply:
      for (uint8_t i = 0; i < d.replyLen; i++) io_.pcWrite(d.reply[i]);
      io_.frame(Frame::SynthToPc, d.reply, d.replyLen);
      break;
    case InterceptAction::Swallow:
      io_.frame(Frame::SwallowedFromPc, frame, CAT_FRAME_LEN);
      break;
    case InterceptAction::Queue:
      enqueue(frame);
      break;
  }
}

// The bus is ours to use for a PC command: nobody holds it, no exchange is in
// flight, and it has been quiet long enough since the last frame (Hamlib leaves
// 50ms after a write; a query's reply is its own acknowledgement, so 10ms).
bool CatBridgeCore::busReadyForReplay(uint32_t now) const {
  return arbiter_.pcMayTransmit() && framer_.pcBusIdle() &&
         (uint32_t)(now - lastBusAt_) >= replayGapMs_;
}

void CatBridgeCore::sendToRadio(const uint8_t frame[CAT_FRAME_LEN], uint32_t now) {
  for (uint8_t i = 0; i < CAT_FRAME_LEN; i++) {
    io_.radioWrite(frame[i]);
    handleEvent(framer_.pcByte(frame[i], now)); // so a query's reply is paired and forwarded
  }
  arbiter_.noteBusActivity(now);
  lastBusAt_ = now;
  replayGapMs_ = catReplyLength(frame[CAT_FRAME_LEN - 1]) ? 10 : ARB_QUIET_MS;
}

// One queued command to the radio, when the bus is ready.
void CatBridgeCore::replayOne(uint32_t now) {
  if (queue_.count() == 0 || !busReadyForReplay(now)) return;
  uint8_t f[CAT_FRAME_LEN];
  queue_.pop(f);
  io_.frame(Frame::Replayed, f, CAT_FRAME_LEN);
  sendToRadio(f, now);
}

bool CatBridgeCore::poll(uint32_t now) {
  bool moved = false;
  updateMode();

  int c;
  if (mode_ == Mode::Normal) {
    // PC -> radio, unless the arbiter is holding the bus. Held bytes wait in the
    // port's RX buffer.
    while (arbiter_.pcMayTransmit() && (c = io_.pcRead()) >= 0) {
      uint8_t b = (uint8_t)c;
      io_.radioWrite(b);
      arbiter_.noteBusActivity(now);
      lastBusAt_ = now;
      handleEvent(framer_.pcByte(b, now));
      moved = true;
    }
  } else if (mode_ == Mode::Intercept || mode_ == Mode::Drain) {
    while ((c = io_.pcRead()) >= 0) {
      interceptByte((uint8_t)c, now);
      moved = true;
    }
    if (asm_.poll(now)) io_.frame(Frame::TornFromPc, nullptr, 0);
  }
  if (mode_ == Mode::Drain) replayOne(now);

  // radio -> PC, except replies to the Arduino's own command.
  while ((c = io_.radioRead()) >= 0) {
    uint8_t b = (uint8_t)c;
    lastBusAt_ = now;
    if (arbiter_.ownsRadioReplies()) {
      arbiter_.radioByte(b, now);
      continue;
    }
    if (mode_ == Mode::Hold || mode_ == Mode::Intercept) {
      io_.frame(Frame::StrayFromRadio, &b, 1); // nobody asked: don't confuse the PC with it
      continue;
    }
    io_.pcWrite(b);
    arbiter_.noteBusActivity(now);
    handleEvent(framer_.radioByte(b, now));
    moved = true;
  }

  handleEvent(framer_.poll(now));

  arbiter_.poll(now, framer_.pcBusIdle());
  if (const uint8_t *cmd = arbiter_.pendingSend()) {
    uint8_t copy[CAT_FRAME_LEN];
    for (uint8_t i = 0; i < CAT_FRAME_LEN; i++) copy[i] = cmd[i];
    for (uint8_t i = 0; i < CAT_FRAME_LEN; i++) io_.radioWrite(copy[i]);
    io_.frame(Frame::ArduinoToRadio, copy, CAT_FRAME_LEN);
    arbiter_.sent(now);
    lastBusAt_ = now;
  }
  return moved;
}
