#pragma once

#include <Arduino.h>

// The tune cycle on real hardware: the TuneCycle state machine (tune.h) wired to
// the CAT bridge, sequencer, AH-4 driver, tune button and tune buzzer (D7).
//
//   button press      start a full tune; press again during one to abort it
//   Serial0 keys:     T  full tune        E  ATU handshake, radio NOT keyed
//                     M  full tune + log the radio's power meter (diagnostic)
//                     P  carrier test: key the radio for 2s with NO tuner, log the meter
//                     R  full tune keying the radio when KEY asserts instead of at START
//                        (the default keys at START so the radio's start-up power
//                        overshoot has settled before the tuner measures)
//                     1/2/3  tune mode AM / FM / CW (default AM)
//                     D  dry run: sequencer + AM mode only, no ATU, no RF
//                     X  abort
//
// While a cycle runs the Arduino owns the CAT bus, so PC traffic waits (a few
// seconds at most); the PC is not yet shown faked mode/PTT replies.

void tuneIoBegin();

// Call every loop(), after ah4IoPoll().
void tuneIoPoll();

// A tune-button press: starts a tune, or aborts one in progress.
void tuneIoButtonPress();

// Offer a Serial0 character; true if it was a tune key.
bool tuneIoHandleChar(char c);

// True while a cycle is running (other bench keys keep out of the way).
bool tuneIoActive();
