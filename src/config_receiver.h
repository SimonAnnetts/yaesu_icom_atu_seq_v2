#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sequencer.h"

// The Serial0 config upload as pure logic (see README "Transport protocol"): watches
// for the line "CONFIG", answers READY, collects exactly one JSON object without
// blocking, then validates, applies and saves it. The ports, the sequencer and the
// EEPROM are behind ConfigHost so the whole thing runs against fakes in host tests.
//
// Damage-proofing, each of which is tested: a wrong first byte, a truncated or
// stalled upload (3s), an oversized one (1024 characters of JSON, whitespace not
// counted), absurd nesting, a damaged or invalid config - every one ends in an ERROR
// reply, the previous config untouched, and the receiver back to idle. An upload is
// refused while a band is transmitting, and again at the end if one has started
// meanwhile.

constexpr size_t CONFIG_JSON_MAX = 1024;
constexpr uint32_t CONFIG_RX_TIMEOUT_MS = 3000;
constexpr uint8_t CONFIG_MAX_DEPTH = 8; // real configs nest 4 deep
// A partly-typed handshake line that nobody finishes within this long is forgotten. Without
// it the leftover of a failed upload (compact JSON has no newlines) would sit in the line
// buffer and silently spoil the next "CONFIG".
constexpr uint32_t CONFIG_LINE_IDLE_MS = 1000;

class ConfigHost {
public:
  enum class Reply : uint8_t {
    Ready,          // "READY"
    Ok,             // "OK"
    Busy,           // ERROR: a band is transmitting
    ExpectedObject, // ERROR: first character was not '{'
    TooLarge,       // ERROR: more than CONFIG_JSON_MAX characters
    TooDeep,        // ERROR: nested too deeply
    Timeout,        // ERROR: the port went quiet mid-upload
    InvalidJson,    // ERROR: not JSON; detail = the parser's message
    InvalidConfig,  // ERROR: JSON but not a usable config; detail = the reason
  };

  virtual bool idleForUpload() = 0;                    // no band sequencing or transmitting
  virtual bool apply(const SequencerConfig &cfg) = 0;  // false: refused (busy)
  virtual void save(const SequencerConfig &cfg) = 0;
  virtual void reply(Reply r, const char *detail) = 0; // detail only for the two Invalid replies

protected:
  ~ConfigHost() {}
};

class ConfigReceiver {
public:
  explicit ConfigReceiver(ConfigHost &host) : host_(host) {}

  // Offer a Serial0 character. Always false while idle (the handshake line is only
  // observed, the character still belongs to the other handlers); true for every
  // character while an upload is in progress.
  bool handleChar(char c, uint32_t now);

  // Call every loop: abandons an upload that has gone quiet.
  void poll(uint32_t now);

  bool receiving() const { return state_ != State::Idle; }

private:
  enum class State : uint8_t { Idle, WaitBrace, Body };

  void startReceive(uint32_t now);
  void receiveChar(char c, uint32_t now);
  void finish();
  void fail(ConfigHost::Reply r, const char *detail = nullptr);
  void reset();

  ConfigHost &host_;
  State state_ = State::Idle;
  char json_[CONFIG_JSON_MAX] = {};
  size_t len_ = 0;
  uint32_t lastByteAt_ = 0;
  uint8_t depth_ = 0;
  bool inString_ = false;
  bool escaped_ = false;

  char line_[8] = {}; // while idle: the line being typed, to spot "CONFIG"
  uint8_t lineLen_ = 0;
  bool lineOverflow_ = false;
  uint32_t lineAt_ = 0; // when the last character of the idle line arrived
  void forgetStaleLine(uint32_t now);
};
