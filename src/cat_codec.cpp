#include "cat_codec.h"

#include "cat_frame.h"

bool catDecodeFreq(const uint8_t bcd[4], uint32_t &hz) {
  uint32_t v = 0;
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t hi = bcd[i] >> 4, lo = bcd[i] & 0x0F;
    if (hi > 9 || lo > 9) return false;
    v = v * 100 + hi * 10 + lo;
  }
  hz = v * 10;
  return true;
}

void catEncodeFreq(uint32_t hz, uint8_t bcd[4]) {
  uint32_t v = hz / 10;
  for (int8_t i = 3; i >= 0; i--) {
    uint8_t pair = v % 100;
    bcd[i] = (pair / 10) << 4 | (pair % 10);
    v /= 100;
  }
}

bool catModeValid(uint8_t mode) {
  switch (mode) {
    case MODE_LSB: case MODE_USB: case MODE_CW: case MODE_CWR: case MODE_AM: case MODE_FM:
    case MODE_CWN: case MODE_CWRN: case MODE_AMN: case MODE_FMN:
      return true;
    default:
      return false;
  }
}

const char *catModeName(uint8_t mode) {
  switch (mode) {
    case MODE_LSB: return "LSB";
    case MODE_USB: return "USB";
    case MODE_CW: return "CW";
    case MODE_CWR: return "CWR";
    case MODE_AM: return "AM";
    case MODE_FM: return "FM";
    case MODE_CWN: return "CWN";
    case MODE_CWRN: return "CWRN";
    case MODE_AMN: return "AMN";
    case MODE_FMN: return "FMN";
    default: return "?";
  }
}

static void block(uint8_t out[5], uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t op) {
  out[0] = a; out[1] = b; out[2] = c; out[3] = d; out[4] = op;
}

void catCmdCatOn(uint8_t out[5]) { block(out, 0, 0, 0, 0, 0x00); }
void catCmdGetFreqMode(uint8_t out[5]) { block(out, 0, 0, 0, 0, CAT_OP_GET_FREQ_MODE_MAIN); }
void catCmdGetTxStatus(uint8_t out[5]) { block(out, 0, 0, 0, 0, CAT_OP_TX_STATUS); }
void catCmdGetRxStatus(uint8_t out[5]) { block(out, 0, 0, 0, 0, CAT_OP_RX_STATUS); }
void catCmdSetMode(uint8_t out[5], uint8_t mode) { block(out, mode, 0, 0, 0, CAT_OP_SET_MODE_MAIN); }
void catCmdPtt(uint8_t out[5], bool on) {
  block(out, 0, 0, 0, 0, on ? CAT_OP_PTT_ON : CAT_OP_PTT_OFF);
}
void catCmdSetFreq(uint8_t out[5], uint32_t hz) {
  catEncodeFreq(hz, out);
  out[4] = 0x01; // set main VFO frequency
}
