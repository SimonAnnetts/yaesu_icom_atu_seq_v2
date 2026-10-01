#pragma once

#include <Arduino.h>

// Phase 0 bench walk-test, driven by single-character commands on Serial0.
// While active, the CAT passthrough is paused (see walktestActive()) and the
// state of the inputs is echoed whenever one changes.
//
//   t  toggle walk-test mode (leaving it restores every output to idle)
//   n  light the next output, all others off (active-high)
//   p  light the previous output
//   0  all outputs off (no output lit)
//   i  pulse TX INHIBIT for TX_INHIBIT_PULSE_MS (never part of n/p stepping)
//
// D9 (ALC PWM) is left alone: it is running Timer2 PWM, so just scope it.
// D4 (TX INHIBIT) is the one output wired to the radio; keep the radio in
// receive while using the test.

// restoreIdle puts every pin back to its idle level (configurePins in main).
void walktestBegin(void (*restoreIdle)());

// Call every loop(); handles Serial0 commands, the inhibit pulse and input echo.
void walktestPoll();

bool walktestActive();
