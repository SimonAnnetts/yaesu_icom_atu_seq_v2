#include "button.h"

void Button::init(bool rawPressed, uint32_t now) {
  stable_ = candidate_ = rawPressed;
  candidateSince_ = now;
  heldMs_ = 0;
}

Button::Event Button::update(bool rawPressed, uint32_t now) {
  if (rawPressed != candidate_) { // any change restarts the timer
    candidate_ = rawPressed;
    candidateSince_ = now;
    return Event::None;
  }
  if (candidate_ == stable_ || (uint32_t)(now - candidateSince_) < BUTTON_DEBOUNCE_MS) {
    return Event::None;
  }
  stable_ = candidate_;
  if (stable_) {
    pressedAt_ = candidateSince_; // the press began when the level first changed
    return Event::Pressed;
  }
  heldMs_ = candidateSince_ - pressedAt_;
  return Event::Released;
}
