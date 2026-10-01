#pragma once

#include <stdint.h>

// Pure LED pattern generator for the tune LED: no pins, time passed in.
//   On       solid
//   Off      dark
//   Success  three slow blinks, then dark
//   Failure  fast blinking for about a second, then dark
class Blinker {
public:
  enum class Pattern : uint8_t { Off, On, Success, Failure };

  void set(Pattern p, uint32_t now) {
    pattern_ = p;
    since_ = now;
  }

  Pattern pattern() const { return pattern_; }

  // True once a timed pattern has played out.
  bool finished(uint32_t now) const {
    switch (pattern_) {
      case Pattern::Success: return (uint32_t)(now - since_) >= SUCCESS_TOTAL_MS;
      case Pattern::Failure: return (uint32_t)(now - since_) >= FAILURE_TOTAL_MS;
      default: return false;
    }
  }

  bool level(uint32_t now) const {
    uint32_t t = now - since_;
    switch (pattern_) {
      case Pattern::On: return true;
      case Pattern::Success: return t < SUCCESS_TOTAL_MS && (t % 300) < 150;
      case Pattern::Failure: return t < FAILURE_TOTAL_MS && (t % 200) < 100;
      default: return false;
    }
  }

  static constexpr uint32_t SUCCESS_TOTAL_MS = 900;  // 3 x (150 on + 150 off)
  static constexpr uint32_t FAILURE_TOTAL_MS = 1000; // 5 x (100 on + 100 off)

private:
  Pattern pattern_ = Pattern::Off;
  uint32_t since_ = 0;
};
