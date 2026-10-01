#pragma once

#include <Arduino.h>

// Sequencer configuration, persisted in EEPROM and loadable over Serial0.
//
// Handshake (see README): the PC sends "CONFIG\n", the firmware answers
// "READY\n" and then collects exactly one JSON object (whitespace outside
// strings is dropped as it arrives, so pretty-printed files fit). It replies
// "OK\n" once the config is validated, applied and saved, or
// "ERROR: <reason>\n" and keeps the previous config. Collection is
// non-blocking and times out, so a half-sent config can't wedge the port or
// stall the sequencer/CAT bridge. Refused while any band is transmitting.

// Load the saved config from EEPROM (or fall back to the built-in defaults) and
// apply it. Call after Serial.begin, before sequencerIoBegin.
void configIoBegin();

// Offer a Serial0 character. Always returns false while idle (the handshake
// line is only observed), true for every character while a config is arriving.
bool configIoHandleChar(char c);

// Call every loop(): enforces the receive timeout.
void configIoPoll();
