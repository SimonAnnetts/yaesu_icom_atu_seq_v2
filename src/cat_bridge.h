#pragma once

#include <Arduino.h>

#include "cat_arbiter.h"

// CAT passthrough between Port 1 (Serial2, PC) and Port 2 (Serial3, radio).
// Every byte is forwarded immediately and unchanged; a copy is fed through the
// CatFramer so whole commands/replies can be seen (and, in later phases,
// intercepted). Frame logging to Serial0 is off by default.
//
// The Arduino can also run its own commands on Port 2 (catBridgeSubmit). They
// wait for the PC's current exchange to finish, hold PC bytes in the Serial2
// RX buffer while they run, and their replies never reach the PC.

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

// Claim the bus for a multi-command job (the tune cycle): PC bytes wait in the
// Serial2 RX buffer until releaseClaim, and commands go out without queueing
// behind PC traffic. See CatArbiter.
bool catBridgeClaim();
CatArbiter::ClaimState catBridgeClaimState();
void catBridgeReleaseClaim();
