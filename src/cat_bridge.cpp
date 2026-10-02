#include "cat_bridge.h"

#include "cat_bridge_core.h"

constexpr uint32_t CAT_BAUD = 57600;
constexpr uint8_t CAT_CONFIG = SERIAL_8N2; // FT-847: 8 data bits, 2 stop, no parity
constexpr int LOG_LINE_MAX = 40;           // skip logging rather than block forwarding

static bool logFrames = false;

namespace {

class SerialIo : public CatBridgeIo {
public:
  int pcRead() override { return Serial2.available() ? Serial2.read() : -1; }
  int radioRead() override { return Serial3.available() ? Serial3.read() : -1; }
  void pcWrite(uint8_t b) override { Serial2.write(b); }
  void radioWrite(uint8_t b) override { Serial3.write(b); }

  void frame(Frame kind, const uint8_t *d, uint8_t n) override {
    if (!logFrames) return;
    // Forwarding matters more than logging: if Serial0's TX buffer is full, drop
    // this line instead of blocking.
    if (Serial.availableForWrite() < LOG_LINE_MAX) return;
    switch (kind) {
      case Frame::PcToRadio: Serial.print(F("PC>RADIO")); break;
      case Frame::RadioToPc: Serial.print(F("RADIO>PC")); break;
      case Frame::StrayFromRadio: Serial.print(F("RADIO>PC stray")); break;
      case Frame::TornFromPc: Serial.print(F("PC>RADIO torn")); break;
      case Frame::ReplyTimeout: Serial.print(F("RADIO>PC timeout")); break;
      case Frame::ArduinoToRadio: Serial.print(F("ARD>RADIO")); break;
      case Frame::SynthToPc: Serial.print(F("PROXY>PC")); break;
      case Frame::SwallowedFromPc: Serial.print(F("PC swallowed")); break;
      case Frame::QueuedFromPc: Serial.print(F("PC queued")); break;
      case Frame::DroppedFromQueue: Serial.print(F("PC queue full, dropped")); break;
      case Frame::Replayed: Serial.print(F("REPLAY>RADIO")); break;
    }
    for (uint8_t i = 0; i < n; i++) {
      Serial.print(' ');
      if (d[i] < 0x10) Serial.print('0');
      Serial.print(d[i], HEX);
    }
    Serial.println();
  }
};

SerialIo io;
CatBridgeCore core(io);

} // namespace

void catBridgeBegin() {
  Serial2.begin(CAT_BAUD, CAT_CONFIG);
  Serial3.begin(CAT_BAUD, CAT_CONFIG);
}

bool catBridgePoll() { return core.poll(millis()); }

bool catBridgeSubmit(const uint8_t cmd[5]) { return core.submit(cmd, millis()); }

bool catBridgeClaim() { return core.claim(millis()); }
CatArbiter::ClaimState catBridgeClaimState() { return core.claimState(); }
void catBridgeReleaseClaim() { core.releaseClaim(millis()); }
void catBridgeSetSnapshot(const CatSnapshot &s) { core.setSnapshot(s); }

bool catBridgeTakeResult(CatArbiter::Result &r, uint8_t *reply, uint8_t &len) {
  return core.takeResult(r, reply, len);
}

void catBridgeHandleChar(char c) {
  if (c == 'c') {
    logFrames = !logFrames;
    Serial.println(logFrames ? F("CAT frame log ON") : F("CAT frame log OFF"));
  }
}
