#pragma once

#include <stdint.h>

// Pure Icom AH-4 START/KEY handshake: no pins, time and the KEY level passed in.
// See README "Icom AH-4 ATU interface". Both lines are active-low on the AH-4;
// this class works in terms of "asserted" and leaves the pin polarity (D12 high
// drives the START opto, D11 low means KEY asserted) to the hardware wrapper.
//
//   begin():  START asserted. The caller keys the radio right after this, so
//             the tuner sees RF while it is busy.
//   The AH-4 asserts KEY ~10ms after START has been held ~100ms: "busy, RF now".
//   KEY releasing again is "tune complete"; 25ms later it is re-sampled, and a
//   bounce back to asserted means the tune failed.
//
// On success START stays asserted until release() (the caller unkeys the radio
// first). On any failure, and on abort(), START is released straight away -
// but never before it has been held AH4_START_MIN_HOLD_MS, since a START
// released inside ~100ms makes the AH-4 toggle bypass instead of tuning.

constexpr uint32_t AH4_START_MIN_HOLD_MS = 150;
constexpr uint32_t AH4_KEY_APPEAR_TIMEOUT_MS = 500; // from START asserted; else "no ATU"
constexpr uint32_t AH4_BUSY_TIMEOUT_MS = 2500;      // from KEY asserted to KEY released
constexpr uint32_t AH4_CONFIRM_MS = 25;

class Ah4Driver {
public:
  enum class Result : uint8_t {
    None,
    Success,  // KEY released and stayed released; START still held until release()
    NoAtu,    // KEY never asserted: no tuner responding
    Timeout,  // KEY asserted but never released
    Bounce,   // KEY came back after releasing: tune failed
    KeyStuck, // KEY already asserted before START: START was not asserted at all
    Aborted,
  };

  // Start a cycle. False if one is already running (including START still
  // being held after a failure). A KEY that is already asserted refuses the
  // cycle with Result::KeyStuck instead.
  bool begin(bool keyAsserted, uint32_t now);

  void poll(bool keyAsserted, uint32_t now);

  // After Success: release START (deferred until the minimum hold has passed).
  void release(uint32_t now);

  // Stop whatever is running and release START (same minimum-hold rule).
  void abort(uint32_t now);

  bool startAsserted() const { return startOn_; }
  bool busy() const { return state_ != State::Idle; }

  // Fetch the outcome once, as soon as it is known (for Success, START may still
  // be held; for failures it is released at the same time or after the minimum hold).
  Result takeResult();

private:
  enum class State : uint8_t { Idle, WaitKey, Busy, Confirm, Holding, Releasing };

  void finish(Result r, uint32_t now);
  void scheduleRelease(uint32_t now);
  void tryRelease(uint32_t now);

  State state_ = State::Idle;
  Result result_ = Result::None;
  bool startOn_ = false;
  uint32_t startedAt_ = 0;
  uint32_t deadline_ = 0; // busy deadline, then confirm deadline
  uint32_t releaseAt_ = 0;
};
