#pragma once

// Hardware side of the sequencer: STBY inputs (interrupt-assisted, debounced),
// output pins, and Serial0 transition logging. The logic lives in sequencer.h.

void sequencerIoBegin();

// Call every loop().
void sequencerIoPoll();

// Force all output pins to be rewritten on the next poll (after something else,
// e.g. the walk-test, has been driving them).
void sequencerIoInvalidate();
