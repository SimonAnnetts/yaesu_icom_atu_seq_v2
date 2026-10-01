#include "button_io.h"

#include <Arduino.h>

#include "button.h"
#include "pins.h"

static Button button;

static bool rawPressed() { return digitalRead(PIN_TUNE_BUTTON) == LOW; }

void buttonIoBegin() { button.init(rawPressed(), millis()); }

void buttonIoPoll() {
  switch (button.update(rawPressed(), millis())) {
    case Button::Event::Pressed:
      Serial.println(F("Tune button pressed"));
      break;
    case Button::Event::Released:
      Serial.print(F("Tune button released after "));
      Serial.print(button.heldMs());
      Serial.println(F("ms"));
      break;
    default:
      break;
  }
}
