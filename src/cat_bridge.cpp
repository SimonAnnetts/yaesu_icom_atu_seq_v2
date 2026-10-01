#include "cat_bridge.h"

#include "cat_frame.h"

constexpr uint32_t CAT_BAUD = 57600;
constexpr uint8_t CAT_CONFIG = SERIAL_8N2; // FT-847: 8 data bits, 2 stop, no parity
constexpr int LOG_LINE_MAX = 40;           // skip logging rather than block forwarding

static CatFramer framer;
static bool logFrames = false;

static void logBytes(const __FlashStringHelper *label, const uint8_t *d, uint8_t n) {
  // Forwarding matters more than logging: if Serial0's TX buffer is full, drop
  // this line instead of blocking.
  if (Serial.availableForWrite() < LOG_LINE_MAX) return;
  Serial.print(label);
  for (uint8_t i = 0; i < n; i++) {
    Serial.print(' ');
    if (d[i] < 0x10) Serial.print('0');
    Serial.print(d[i], HEX);
  }
  Serial.println();
}

static void handleEvent(const CatEvent &ev) {
  if (!logFrames) return;
  switch (ev.kind) {
    case CatEvent::Command: logBytes(F("PC>RADIO"), ev.data, ev.len); break;
    case CatEvent::Reply: logBytes(F("RADIO>PC"), ev.data, ev.len); break;
    case CatEvent::StrayReply: logBytes(F("RADIO>PC stray"), ev.data, ev.len); break;
    case CatEvent::CommandAbandoned: logBytes(F("PC>RADIO torn"), ev.data, ev.len); break;
    case CatEvent::ReplyAbandoned: logBytes(F("RADIO>PC timeout"), ev.data, ev.len); break;
    default: break;
  }
}

void catBridgeBegin() {
  Serial2.begin(CAT_BAUD, CAT_CONFIG);
  Serial3.begin(CAT_BAUD, CAT_CONFIG);
}

bool catBridgePoll() {
  bool moved = false;
  while (Serial2.available()) {
    uint8_t b = Serial2.read();
    Serial3.write(b);
    handleEvent(framer.pcByte(b, millis()));
    moved = true;
  }
  while (Serial3.available()) {
    uint8_t b = Serial3.read();
    Serial2.write(b);
    handleEvent(framer.radioByte(b, millis()));
    moved = true;
  }
  handleEvent(framer.poll(millis()));
  return moved;
}

void catBridgeHandleChar(char c) {
  if (c == 'c') {
    logFrames = !logFrames;
    Serial.println(logFrames ? F("CAT frame log ON") : F("CAT frame log OFF"));
  }
}
