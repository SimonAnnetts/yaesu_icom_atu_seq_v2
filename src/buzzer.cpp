#include "buzzer.h"

#include "pins.h"

constexpr uint32_t BUZZER_HZ = 2048;
// PWM duty, 1..50%. 50 is loudest; lower is quieter and draws less average current
// from a magnetic buzzer (a software volume knob). Use a series resistor in any case
// (see README): a 40 ohm coil straight from the pin would draw ~125mA, over the
// Mega's 40mA absolute maximum.
constexpr uint32_t BUZZER_DUTY_PERCENT = 50;
// Fast PWM, mode 14: TOP = ICR4, no prescaler, f = F_CPU / (TOP + 1).
constexpr uint32_t BUZZER_TOP = F_CPU / BUZZER_HZ - 1;
constexpr uint32_t BUZZER_ACTUAL_HZ = F_CPU / (BUZZER_TOP + 1);

static_assert(PIN_TUNE_LED == 7, "buzzer drive below assumes D7 = OC4B (Timer4)");
static_assert(BUZZER_DUTY_PERCENT >= 1 && BUZZER_DUTY_PERCENT <= 50, "duty above 50% is no louder");
static_assert(BUZZER_TOP < 65536, "Timer4 TOP must fit in 16 bits");
static_assert(BUZZER_ACTUAL_HZ > BUZZER_HZ - 5 && BUZZER_ACTUAL_HZ < BUZZER_HZ + 5,
              "buzzer frequency off target");

void buzzerBegin() {
  digitalWrite(PIN_TUNE_LED, LOW);
  pinMode(PIN_TUNE_LED, OUTPUT);
  ICR4 = BUZZER_TOP;
  OCR4B = (BUZZER_TOP + 1) * BUZZER_DUTY_PERCENT / 100;
  TCCR4A = _BV(WGM41);    // OC4B disconnected until buzzerSet(true)
  TCCR4B = _BV(WGM43) | _BV(WGM42) | _BV(CS40);
}

void buzzerSet(bool on) {
  if (on) {
    TCCR4A |= _BV(COM4B1); // non-inverting PWM on OC4B (D7)
  } else {
    TCCR4A &= ~_BV(COM4B1);
    PORTH &= ~_BV(PH4); // D7 low: no DC across the buzzer
  }
}
