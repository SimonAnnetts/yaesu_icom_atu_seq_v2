#pragma once

#include <Arduino.h>

// ALC injection: a charge pump (Timer2 PWM on D9, ~14.9kHz) makes about -4V, and an
// opto-isolator (LED on D10 via 470 ohm) switches it onto the radio's ALC line,
// which pulls the carrier down (0V = no effect, -4V = maximum reduction). The pump
// runs all the time at 50% duty; the gate decides whether the radio sees it. (Duty
// is not a control: on the real circuit 5% to 95% moved the output only ~0.2V, as
// with any diode charge pump. To back the voltage off, use a pot in the circuit.)
//
// Fail-safe: a reset or a hang turns the gate off (the pin goes to input, the LED
// dark), and a gate switched on by hand releases itself after 30s.
//
// Bench key (when no tune is running):  g  gate on/off

void alcBegin();

// Restart the pump and drop the gate: after something (the walk-test) has driven
// the pins directly. configurePins() disconnects D9's PWM output as a side effect.
void alcResync();

// Call every loop(): releases a gate left on by hand.
void alcPoll();

// The tune cycle's switch. Always releases to off at the end of a cycle.
void alcTuneSet(bool on);

bool alcGate();

bool alcHandleChar(char c);
