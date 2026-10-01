#pragma once

#include <Arduino.h>

#include "ah4.h"

// Pin side of the AH-4 handshake: D12 drives the START opto (high = START
// asserted at the tuner), D11 reads the KEY opto (low = KEY asserted). The
// logic is in ah4.h; this adds the pins, logging, and Serial0 bench keys:
//
//   h  run a START/KEY handshake with NO radio keyed (the tuner sees no RF, so
//      it just lets go of KEY again after a few hundred ms - this checks the
//      wiring and shows the tuner's timing; it is not a tune)
//   j  abort / release START now
//   K  toggle a fake KEY (as if the tuner asserted it) to test with no tuner

void ah4IoBegin();

// Call every loop().
void ah4IoPoll();

// Offer a Serial0 character; true if it was an AH-4 key.
bool ah4IoHandleChar(char c);

// Re-assert the output pin from the driver state (after the walk-test touched it).
void ah4IoResync();

// For the tune cycle (and bench keys): same driver the keys use.
bool ah4Begin();
void ah4Abort();
bool ah4StartAsserted();
// Final outcome once known (see Ah4Driver::takeResult).
Ah4Driver::Result ah4TakeResult();
