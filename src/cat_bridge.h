#pragma once

#include <Arduino.h>

#include "cat_arbiter.h"
#include "cat_intercept.h"

// CAT passthrough between Port 1 (Serial2, PC) and Port 2 (Serial3, radio), byte
// for byte in both directions. The logic lives in cat_bridge_core.h (host-tested);
// this wires it to the serial ports and logs frames to Serial0 (off by default).
//
// The Arduino can also run its own commands on Port 2 (catBridgeSubmit), and hold
// the bus for a whole job (claim). While it holds it the PC is still answered from
// a snapshot of the radio (catBridgeSetSnapshot) - see CatBridgeCore - so a tune
// is invisible to it.

void catBridgeBegin();

// Call every loop(). Returns true if any byte moved in either direction.
bool catBridgePoll();

// Serial0 command handling: 'c' toggles frame logging.
void catBridgeHandleChar(char c);

// Queue one 5-byte command for the radio. False if a transaction is already
// running. The outcome arrives later via catBridgeTakeResult.
bool catBridgeSubmit(const uint8_t cmd[5]);

// True once per finished transaction; reply gets up to 5 bytes.
bool catBridgeTakeResult(CatArbiter::Result &r, uint8_t *reply, uint8_t &len);

// Claim the bus for a multi-command job (the tune cycle). See CatArbiter.
bool catBridgeClaim();
CatArbiter::ClaimState catBridgeClaimState();
void catBridgeReleaseClaim();

// The radio's state before the tune: the PC is answered from it until the claim ends.
void catBridgeSetSnapshot(const CatSnapshot &s);
