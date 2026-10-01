#pragma once

#include <stdint.h>

// Pure FT-847 CAT value helpers: BCD frequency, mode bytes, command builders.

// Frequency is 4 packed-BCD bytes in units of 10Hz, most significant first:
// 43 21 00 00 = 432.1000MHz.
bool catDecodeFreq(const uint8_t bcd[4], uint32_t &hz); // false if a nibble is > 9
void catEncodeFreq(uint32_t hz, uint8_t bcd[4]);        // rounds down to 10Hz

// Mode bytes, as used both by Set Mode (0x07) and the 5th byte of a 0x03 reply.
// The 0x80 bit marks the narrow variants; keeping the raw byte makes a
// read-then-restore exact.
enum CatMode : uint8_t {
  MODE_LSB = 0x00,
  MODE_USB = 0x01,
  MODE_CW = 0x02,
  MODE_CWR = 0x03,
  MODE_AM = 0x04,
  MODE_FM = 0x08,
  MODE_CWN = 0x82,
  MODE_CWRN = 0x83,
  MODE_AMN = 0x84,
  MODE_FMN = 0x88,
};
bool catModeValid(uint8_t mode);
const char *catModeName(uint8_t mode); // "?" if invalid

// Transmit status (0xF7 reply): bit 7 set means NOT transmitting.
inline bool catTxStatusTransmitting(uint8_t status) { return !(status & 0x80); }

// Command builders, each filling a 5-byte block (opcode last).
void catCmdCatOn(uint8_t out[5]);
void catCmdGetFreqMode(uint8_t out[5]); // reply: 4 freq bytes + mode byte
void catCmdGetTxStatus(uint8_t out[5]); // reply: 1 byte
void catCmdSetMode(uint8_t out[5], uint8_t mode);
void catCmdSetFreq(uint8_t out[5], uint32_t hz);
void catCmdPtt(uint8_t out[5], bool on);
