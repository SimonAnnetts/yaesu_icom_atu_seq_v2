#pragma once

// Tune button (D6, active-low: pressed pulls the pin to ground). Presses are
// debounced and logged on Serial0.

void buttonIoBegin();

// Call every loop(). True once per debounced press.
bool buttonIoPoll();
