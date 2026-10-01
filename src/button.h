#pragma once

#include <stdint.h>

// Pure debounced push-button: no pins, time passed in. A raw reading must stay
// the same for DEBOUNCE_MS before it is accepted, so contact bounce and noise
// can't produce extra events. One event per press and one per release; holding
// the button does not repeat.

constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;

class Button {
public:
  enum class Event : uint8_t { None, Pressed, Released };

  // Take the starting level as already stable, so a button held down at boot
  // doesn't report a press.
  void init(bool rawPressed, uint32_t now);

  // Feed the raw (undebounced) reading each loop. After a Released event,
  // heldMs() is how long the debounced press lasted.
  Event update(bool rawPressed, uint32_t now);

  bool pressed() const { return stable_; }
  uint32_t heldMs() const { return heldMs_; }

private:
  bool stable_ = false;  // debounced level
  bool candidate_ = false; // raw level currently being timed
  uint32_t candidateSince_ = 0;
  uint32_t pressedAt_ = 0;
  uint32_t heldMs_ = 0;
};
