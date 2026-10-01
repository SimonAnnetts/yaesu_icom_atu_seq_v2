#pragma once

#include <stdint.h>

#include "cat_frame.h"

// Pure bus arbitration for Arduino-originated CAT commands on Port 2. The
// bridge asks the arbiter whether the PC may transmit; the arbiter claims the
// bus only when no PC exchange is in flight, holds PC bytes back (they wait in
// the Serial2 RX buffer, nothing is dropped) while it runs its own command, and
// keeps a short quiet gap on either side of it, as the FT-847 dislikes
// back-to-back writes. With no PC attached the bus is simply always free.
//
// A longer-lived claim() holds the bus for a whole multi-command job (the tune
// cycle): once Held, PC bytes wait in the Serial2 RX buffer and submit() sends
// at once, spaced from the last bus activity by ARB_QUIET_MS, instead of queueing
// behind PC traffic each time. releaseClaim() hands the bus back after a quiet gap.
//
// Flow: submit() -> WaitBus -> (PC exchange finishes; PC held) -> quiet gap ->
// Sending -> WaitReply (if the opcode has a reply) -> Hold (quiet gap) -> Idle.

constexpr uint32_t ARB_QUIET_MS = 50;       // bus quiet before sending and after finishing
constexpr uint32_t ARB_BUS_TIMEOUT_MS = 2000; // give up waiting for the PC to go quiet
constexpr uint32_t ARB_REPLY_TIMEOUT_MS = 500;
constexpr uint32_t ARB_CLAIM_TIMEOUT_MS = 2000; // give up waiting to claim the bus
constexpr uint32_t ARB_CLAIM_MAX_MS = 60000;    // a forgotten claim releases itself (outlasts a whole tune cycle)

class CatArbiter {
public:
  enum class Result : uint8_t {
    None,
    Ok,         // sent; reply (if one was expected) is in reply/len
    NoReply,    // radio did not answer in time
    BusTimeout, // the bus never became free
  };

  // Start a transaction. False if one is already running (or a claim is still
  // being acquired).
  bool submit(const uint8_t cmd[CAT_FRAME_LEN], uint32_t now);

  // Claim the bus for a multi-command job. False if already claimed/pending.
  enum class ClaimState : uint8_t { None, Pending, Held, Failed };
  bool claim(uint32_t now);
  void releaseClaim(uint32_t now); // also clears a Failed claim
  ClaimState claimState() const { return claim_; }

  // Note every byte seen on the bus that the arbiter did not send itself.
  void noteBusActivity(uint32_t now) { lastActivity_ = now; }

  // pcBusIdle: the PC has no partial command and no reply outstanding.
  void poll(uint32_t now, bool pcBusIdle);

  // True while PC bytes may be read and forwarded.
  bool pcMayTransmit() const;

  // True while radio bytes belong to the arbiter's command, not the PC.
  bool ownsRadioReplies() const { return state_ == State::WaitReply; }
  void radioByte(uint8_t b, uint32_t now);

  // Non-null when the command should be written to the radio now; call sent() after.
  const uint8_t *pendingSend() const { return state_ == State::Sending ? cmd_ : nullptr; }
  void sent(uint32_t now);

  bool busy() const { return state_ != State::Idle; }

  // Fetch the finished transaction's result once. reply gets up to CAT_FRAME_LEN bytes.
  bool takeResult(Result &r, uint8_t *reply, uint8_t &len);

private:
  enum class State : uint8_t { Idle, WaitBus, Sending, WaitReply, Hold };

  void finish(Result r, uint32_t now);

  State state_ = State::Idle;
  bool gateClosed_ = false; // PC held while we wait out the quiet gap
  ClaimState claim_ = ClaimState::None;
  uint32_t claimAt_ = 0; // when claiming started, then when it was acquired
  uint8_t cmd_[CAT_FRAME_LEN] = {};
  uint8_t replyWanted_ = 0;
  uint8_t reply_[CAT_FRAME_LEN] = {};
  uint8_t replyGot_ = 0;
  uint32_t submittedAt_ = 0;
  uint32_t lastActivity_ = 0;
  uint32_t deadline_ = 0; // reply deadline, then hold-until
  Result result_ = Result::None;
};
