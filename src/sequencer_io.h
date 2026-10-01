#pragma once

#include "sequencer.h"

// Hardware side of the sequencer: STBY inputs (interrupt-assisted, debounced),
// output pins, and Serial0 transition logging. The logic lives in sequencer.h.

void sequencerIoBegin();

// Call every loop().
void sequencerIoPoll();

// Force all output pins to be rewritten on the next poll (after something else,
// e.g. the walk-test, has been driving them).
void sequencerIoInvalidate();

// The configuration the sequencer is running with (built-in defaults until
// something else is applied).
const SequencerConfig &sequencerIoConfig();

// Switch to a new configuration. Refused (false) while any band is mid-sequence
// or transmitting, so a config change can never disturb a live transmission.
bool sequencerIoApplyConfig(const SequencerConfig &cfg);
