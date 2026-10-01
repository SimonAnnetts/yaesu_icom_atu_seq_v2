#pragma once

#include <stdint.h>

// Pure FT-847 CAT framing: no Arduino, time passed in. The bridge feeds every
// byte it forwards through here so the rest of the firmware can see whole
// commands and replies. Protocol per the FT-847 manual (CAT System Programming)
// and Hamlib's ft847.c:
//   - PC -> radio: always a 5-byte block, opcode last, up to 200ms between bytes
//   - radio -> PC: replies only to E7 (RX status) and F7 (TX status), 1 byte;
//     and 03/13/23 (freq + mode: main / sat RX / sat TX), 5 bytes.
//     Every other command, including all the "set" commands, gets no reply.

constexpr uint8_t CAT_FRAME_LEN = 5;
constexpr uint32_t CAT_BYTE_TIMEOUT_MS = 200;   // manual: max gap between command bytes
constexpr uint32_t CAT_REPLY_TIMEOUT_MS = 2000; // Hamlib's default FT-847 read timeout

constexpr uint8_t CAT_OP_PTT_ON = 0x08;
constexpr uint8_t CAT_OP_PTT_OFF = 0x88;
constexpr uint8_t CAT_OP_SET_MODE_MAIN = 0x07;
constexpr uint8_t CAT_OP_RX_STATUS = 0xE7;
constexpr uint8_t CAT_OP_TX_STATUS = 0xF7;
constexpr uint8_t CAT_OP_GET_FREQ_MODE_MAIN = 0x03;

// Number of bytes the radio sends back for a command with this opcode (0 = none).
// Unknown opcodes are treated as "no reply".
uint8_t catReplyLength(uint8_t opcode);

struct CatEvent {
  enum Kind : uint8_t {
    None,
    Command,          // complete PC command; data = 5 bytes
    Reply,            // complete radio reply to `opcode`; data = len bytes
    StrayReply,       // radio byte with no request outstanding; data = 1 byte
    CommandAbandoned, // partial PC command dropped after a timeout; data = len bytes
    ReplyAbandoned,   // radio never finished a reply; data = len bytes received so far
  };
  Kind kind = None;
  uint8_t opcode = 0; // the request's opcode (Reply, ReplyAbandoned)
  uint8_t len = 0;
  uint8_t data[CAT_FRAME_LEN] = {};
};

class CatFramer {
public:
  CatEvent pcByte(uint8_t b, uint32_t now);
  CatEvent radioByte(uint8_t b, uint32_t now);

  // Call every loop: abandons a stalled partial command or an overdue reply.
  // Returns at most one event per call.
  CatEvent poll(uint32_t now);

  bool replyPending() const { return pendingLen_ != 0; }

  // No PC command partly sent and no reply outstanding: a safe moment to take the bus.
  bool pcBusIdle() const { return cmdLen_ == 0 && pendingLen_ == 0; }

private:
  uint8_t cmd_[CAT_FRAME_LEN] = {};
  uint8_t cmdLen_ = 0;
  uint32_t lastCmdByteAt_ = 0;

  uint8_t pendingOpcode_ = 0;
  uint8_t pendingLen_ = 0; // bytes the reply should have; 0 = nothing outstanding
  uint8_t reply_[CAT_FRAME_LEN] = {};
  uint8_t replyGot_ = 0;
  uint32_t replyDeadline_ = 0;
};
