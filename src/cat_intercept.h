#pragma once

#include <stdint.h>

#include "cat_frame.h"

// Pure pieces of the PC-transparency layer (README "PC transparency during a
// tune cycle"). While the tune cycle owns the radio, the PC is still polled and
// answered - from a snapshot of the radio taken before the tune - so it never
// sees the AM mode, the keyed transmitter, or a gap in its replies.

// The radio as it was before the tune started, from the cycle's own queries.
struct CatSnapshot {
  bool haveFreqMode = false;
  uint8_t freqMode[CAT_FRAME_LEN] = {}; // a 0x03 reply: 4 BCD frequency bytes + mode
  bool haveTx = false;
  uint8_t tx = 0; // 0xF7 reply: PTT bit set = receiving
  bool haveRx = false;
  uint8_t rx = 0; // 0xE7 reply
};

enum class InterceptAction : uint8_t {
  Reply,   // answer the PC from the snapshot
  Swallow, // drop silently (it would conflict with the tune)
  Queue,   // hold it and send it to the radio once the tune is over
};

struct InterceptDecision {
  InterceptAction action = InterceptAction::Queue;
  uint8_t reply[CAT_FRAME_LEN] = {};
  uint8_t replyLen = 0;
};

// What to do with one complete command from the PC while a tune is running:
//   0x03 freq/mode (main)  -> snapshot freq and ORIGINAL mode
//   0xF7 TX status         -> snapshot (not transmitting)
//   0xE7 RX status         -> snapshot
//   0x08 / 0x88 PTT        -> swallowed: it would key or unkey against the tune
//   everything else        -> queued, in order, and replayed after the tune:
//                             set freq / mode / CAT on-off, sat VFO queries, ...
// A query whose snapshot field is missing is queued too (answered live later).
InterceptDecision catIntercept(const CatSnapshot &snap, const uint8_t cmd[CAT_FRAME_LEN]);

// Assembles a byte stream from the PC into 5-byte commands, dropping a partial
// command that stalls longer than the protocol allows (CAT_BYTE_TIMEOUT_MS).
class CatFrameAssembler {
public:
  // True when this byte completed a command (copied to out).
  bool push(uint8_t b, uint32_t now, uint8_t out[CAT_FRAME_LEN]);
  // True if a stalled partial command was just dropped.
  bool poll(uint32_t now);
  bool idle() const { return len_ == 0; }

private:
  uint8_t buf_[CAT_FRAME_LEN] = {};
  uint8_t len_ = 0;
  uint32_t lastAt_ = 0;
};

// Commands held back during a tune, replayed in arrival order afterwards. When it
// is full the OLDEST entry is dropped, so the latest intent (the newest mode or
// frequency the user asked for) always survives.
constexpr uint8_t CAT_REPLAY_MAX = 8;

class CatReplayQueue {
public:
  // True if an old entry had to be dropped to make room.
  bool push(const uint8_t cmd[CAT_FRAME_LEN]);
  bool pop(uint8_t out[CAT_FRAME_LEN]);
  uint8_t count() const { return count_; }
  uint16_t dropped() const { return dropped_; }

private:
  uint8_t buf_[CAT_REPLAY_MAX][CAT_FRAME_LEN] = {};
  uint8_t head_ = 0;
  uint8_t count_ = 0;
  uint16_t dropped_ = 0;
};
