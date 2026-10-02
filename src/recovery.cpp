#include "recovery.h"

#include "cat_codec.h"

void recoveryEncode(uint8_t mode, uint8_t out[RECOVERY_RECORD_LEN]) {
  out[0] = RECOVERY_MAGIC;
  out[1] = mode;
  out[2] = (uint8_t)~mode;
}

bool recoveryDecode(const uint8_t in[RECOVERY_RECORD_LEN], uint8_t &mode) {
  if (in[0] != RECOVERY_MAGIC || (uint8_t)(in[1] ^ in[2]) != 0xFF) return false;
  mode = in[1];
  return true;
}

uint8_t recoveryScript(uint8_t mode, RecoveryStep out[4]) {
  uint8_t n = 0;
  catCmdCatOn(out[n].cmd);
  out[n++].delayAfterMs = 60;
  catCmdPtt(out[n].cmd, false);
  out[n++].delayAfterMs = 60;
  if (catModeValid(mode)) {
    catCmdSetMode(out[n].cmd, mode);
    out[n++].delayAfterMs = 60;
  }
  catCmdPtt(out[n].cmd, false); // again, once the radio has had time to settle
  out[n++].delayAfterMs = 60;
  return n;
}
