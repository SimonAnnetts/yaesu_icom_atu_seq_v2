#pragma once

#include <stdint.h>

// Pure Icom AH-4 START/KEY handshake: no pins, time and the KEY level passed in.
// See README "Icom AH-4 ATU interface". Both lines are active-low on the tuner;
// this class works in terms of "asserted" and leaves the pin polarity (D12 high
// drives the START opto, D11 low means KEY asserted) to the hardware wrapper.
//
// It behaves like an Icom radio: START is asserted for AH4_START_HOLD_MS and
// then released, whatever KEY is doing (K9EQ: START low ~560ms). Tuners differ
// in when KEY follows -
//   genuine AH-4 (K9EQ):  KEY ~300ms after START asserts, i.e. during the hold
//   Alinco EDX-2 (bench): KEY ~35ms after START is RELEASED, however long it
//                         was held (800ms -> 833ms, 2000ms -> 2036ms)
// - so KEY is accepted anywhere from START asserting to AH4_KEY_APPEAR_TIMEOUT_MS
// after START releases.
//
// The caller must NOT key the radio before KEY asserts (the tuner has not yet
// switched RF through its power divider): when keySeen() turns true, key PTT.
// KEY releasing is "tune complete"; if it re-asserts within AH4_CONFIRM_MS the
// tune failed (the AH-4's not-tuned signal is KEY released 20ms, asserted 200ms,
// released). Note that KEY releasing with no RF applied is NOT a tune: the
// EDX-2 simply lets go of KEY after ~326ms - only the caller knows it keyed RF.
//
// START is never released earlier than AH4_START_MIN_HOLD_MS, even on abort or
// failure: a pulse shorter than ~100ms is the tuner's reset command.

constexpr uint32_t AH4_START_HOLD_MS = 560;
constexpr uint32_t AH4_START_MIN_HOLD_MS = 150;
constexpr uint32_t AH4_KEY_APPEAR_TIMEOUT_MS = 500; // after START is released; else "no ATU"
constexpr uint32_t AH4_BUSY_TIMEOUT_MS = 2500;      // from KEY asserted to KEY released
constexpr uint32_t AH4_CONFIRM_MS = 50; // must comfortably exceed the 20ms not-tuned gap

class Ah4Driver {
public:
  enum class Result : uint8_t {
    None,
    Success,    // KEY released and stayed released
    NoAtu,      // KEY never asserted: no tuner responding
    Timeout,    // KEY asserted but never released
    TuneFailed, // KEY re-asserted after releasing: the AH-4's not-tuned signal
    KeyStuck,   // KEY already asserted before START: START was not asserted at all
    Aborted,
  };

  // Start a cycle. False if one is already running or START is still held. A KEY
  // that is already asserted refuses the cycle with Result::KeyStuck instead.
  bool begin(bool keyAsserted, uint32_t now);

  void poll(bool keyAsserted, uint32_t now);

  // Stop whatever is running; START is released at once if it has been held
  // long enough, otherwise as soon as it has.
  void abort(uint32_t now);

  bool startAsserted() const { return startOn_; }
  // KEY has asserted at some point this cycle: the cue to key the radio.
  bool keySeen() const { return keySeen_; }
  // A cycle is in progress, or START is still being held after one.
  bool busy() const { return state_ != State::Idle || startOn_; }

  // Fetch the outcome once, as soon as it is known (START may still be held).
  Result takeResult();

private:
  enum class State : uint8_t { Idle, WaitKey, Busy, Confirm };

  void finish(Result r, uint32_t now, bool releaseStartEarly);
  void shortenStartHold(uint32_t now);

  State state_ = State::Idle;
  Result result_ = Result::None;
  bool startOn_ = false;
  bool keySeen_ = false;
  uint32_t startedAt_ = 0;
  uint32_t startReleaseAt_ = 0;
  uint32_t keyDeadline_ = 0;
  uint32_t deadline_ = 0; // busy deadline, then confirm deadline
};
