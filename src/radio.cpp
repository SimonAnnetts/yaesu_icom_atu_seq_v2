#include "radio.h"

#include "cat_bridge.h"
#include "cat_codec.h"
#include "sequencer.h"
#include "sequencer_io.h"

constexpr uint32_t PTT_ARM_MS = 5000;
constexpr uint32_t PTT_MAX_ON_MS = 3000;

enum class Op : uint8_t { None, CatOn, FreqMode, TxStatus, SetMode, PttOn, PttOff };
static Op inFlight = Op::None;

static bool haveMode = false;
static uint8_t lastMode = 0;

static uint32_t pttArmedUntil = 0; // 0 = not armed
static uint32_t pttOffAt = 0;      // 0 = PTT not on
static bool pttOffWanted = false;  // release command still waiting to be accepted

static void printMHz(uint32_t hz) {
  Serial.print(hz / 1000000);
  Serial.print('.');
  uint32_t frac = hz % 1000000;
  for (uint32_t d = 100000; d > 0; d /= 10) {
    Serial.print(frac / d % 10);
  }
}

static bool start(Op op, const uint8_t cmd[5]) {
  if (inFlight != Op::None || !catBridgeSubmit(cmd)) {
    Serial.println(F("radio: busy, try again"));
    return false;
  }
  inFlight = op;
  return true;
}

static void reportFreqMode(const uint8_t *r) {
  uint32_t hz;
  if (!catDecodeFreq(r, hz)) {
    Serial.println(F("radio: bad frequency BCD in reply"));
    return;
  }
  haveMode = true;
  lastMode = r[4];
  int8_t band = bandForFrequency(sequencerIoConfig(), hz);
  Serial.print(F("radio: "));
  printMHz(hz);
  Serial.print(F(" MHz "));
  Serial.print(catModeName(r[4]));
  if (!catModeValid(r[4])) {
    Serial.print(F(" (0x"));
    Serial.print(r[4], HEX);
    Serial.print(')');
  }
  Serial.print(F(", band index "));
  Serial.println(band);
}

static void handleResult(Op op, CatArbiter::Result res, const uint8_t *r, uint8_t len) {
  if (res == CatArbiter::Result::BusTimeout) {
    Serial.println(F("radio: bus never went quiet, command not sent"));
    if (op == Op::PttOff) pttOffWanted = true; // keep trying to release
    return;
  }
  if (res == CatArbiter::Result::NoReply) {
    Serial.print(F("radio: no reply (got "));
    Serial.print(len);
    Serial.println(F(" bytes)"));
    return;
  }
  switch (op) {
    case Op::FreqMode: reportFreqMode(r); break;
    case Op::TxStatus:
      Serial.println(catTxStatusTransmitting(r[0]) ? F("radio: transmitting")
                                                   : F("radio: not transmitting"));
      break;
    case Op::CatOn: Serial.println(F("radio: CAT on sent")); break;
    case Op::SetMode: Serial.println(F("radio: mode sent")); break;
    case Op::PttOn:
      Serial.println(F("radio: PTT ON sent"));
      pttOffAt = millis() + PTT_MAX_ON_MS;
      break;
    case Op::PttOff:
      Serial.println(F("radio: PTT OFF sent"));
      pttOffAt = 0;
      break;
    default: break;
  }
}

static void sendSetMode(uint8_t mode) {
  uint8_t cmd[5];
  catCmdSetMode(cmd, mode);
  if (start(Op::SetMode, cmd)) {
    Serial.print(F("radio: setting mode "));
    Serial.println(catModeName(mode));
  }
}

void radioHandleChar(char c) {
  uint8_t cmd[5];
  if (c != 'k') pttArmedUntil = 0; // any other key disarms PTT
  switch (c) {
    case '?':
      Serial.println(F("radio keys: f freq+mode, x tx status, o CAT on, u/l/a set USB/LSB/AM, "
                       "m restore last-read mode, k k key PTT (3s max), z PTT off"));
      break;
    case 'f': catCmdGetFreqMode(cmd); start(Op::FreqMode, cmd); break;
    case 'x': catCmdGetTxStatus(cmd); start(Op::TxStatus, cmd); break;
    case 'o': catCmdCatOn(cmd); start(Op::CatOn, cmd); break;
    case 'u': sendSetMode(MODE_USB); break;
    case 'l': sendSetMode(MODE_LSB); break;
    case 'a': sendSetMode(MODE_AM); break;
    case 'm':
      if (haveMode) sendSetMode(lastMode);
      else Serial.println(F("radio: no mode read yet, press f first"));
      break;
    case 'k':
      if (pttArmedUntil == 0 || (int32_t)(millis() - pttArmedUntil) >= 0) {
        pttArmedUntil = millis() + PTT_ARM_MS;
        Serial.println(F("radio: PTT armed - press k again within 5s to KEY the radio (dummy load!)"));
      } else {
        pttArmedUntil = 0;
        catCmdPtt(cmd, true);
        if (start(Op::PttOn, cmd)) Serial.println(F("radio: keying PTT"));
      }
      break;
    case 'z': pttOffWanted = true; break;
  }
}

void radioPoll() {
  CatArbiter::Result res;
  uint8_t reply[CAT_FRAME_LEN];
  uint8_t len;
  // Only collect a result if one of these debug keys started the command: the
  // tune cycle shares the arbiter, and its results must not be taken from it.
  if (inFlight != Op::None && catBridgeTakeResult(res, reply, len)) {
    Op op = inFlight;
    inFlight = Op::None;
    handleResult(op, res, reply, len);
  }

  // Auto-release: PTT must never stay on because a key press was forgotten.
  if (pttOffAt && (int32_t)(millis() - pttOffAt) >= 0) pttOffWanted = true;
  if (pttOffWanted && inFlight == Op::None) {
    uint8_t cmd[5];
    catCmdPtt(cmd, false);
    if (start(Op::PttOff, cmd)) pttOffWanted = false;
  }
}
