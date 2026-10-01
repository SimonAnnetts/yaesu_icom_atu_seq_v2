#pragma once

#include <Arduino.h>

// CAT passthrough between Port 1 (Serial2, PC) and Port 2 (Serial3, radio).
// Every byte is forwarded immediately and unchanged; a copy is fed through the
// CatFramer so whole commands/replies can be seen (and, in later phases,
// intercepted). Frame logging to Serial0 is off by default.

void catBridgeBegin();

// Call every loop(). Returns true if any byte moved in either direction.
bool catBridgePoll();

// Serial0 command handling: 'c' toggles frame logging.
void catBridgeHandleChar(char c);
