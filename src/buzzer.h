#pragma once

#include <Arduino.h>

// Tune indicator: a passive buzzer on D7, driven at about 2048Hz by Timer4's
// hardware PWM (D7 is OC4B on the Mega), so it costs no CPU time. Timer4 is
// used directly rather than tone(): on the Mega tone() takes Timer2 first, and
// Timer2 generates the ALC charge-pump PWM on D9.
//
// buzzerSet(false) disconnects the PWM output and leaves D7 low, so a
// silent buzzer never carries DC.

void buzzerBegin();
void buzzerSet(bool on);
