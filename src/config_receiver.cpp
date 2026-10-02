#include "config_receiver.h"

#include <ArduinoJson.h>
#include <string.h>

#include "config_json.h"

using Reply = ConfigHost::Reply;

void ConfigReceiver::reset() {
  state_ = State::Idle;
  len_ = 0;
  depth_ = 0;
  inString_ = false;
  escaped_ = false;
}

void ConfigReceiver::fail(Reply r, const char *detail) {
  host_.reply(r, detail);
  reset();
}

void ConfigReceiver::startReceive(uint32_t now) {
  if (!host_.idleForUpload()) {
    fail(Reply::Busy);
    return;
  }
  reset();
  state_ = State::WaitBrace;
  lastByteAt_ = now;
  host_.reply(Reply::Ready, nullptr);
}

void ConfigReceiver::finish() {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, json_, len_);
  if (e) {
    fail(Reply::InvalidJson, e.c_str());
    return;
  }
  SequencerConfig cfg;
  char err[96];
  if (!configFromJson(doc, cfg, err, sizeof err)) {
    fail(Reply::InvalidConfig, err);
    return;
  }
  if (!host_.apply(cfg)) { // a band started transmitting while we were receiving
    fail(Reply::Busy);
    return;
  }
  host_.save(cfg);
  host_.reply(Reply::Ok, nullptr);
  reset();
}

// One character of the config text. Returns once the object is complete or on error.
void ConfigReceiver::receiveChar(char c, uint32_t now) {
  lastByteAt_ = now;
  bool space = c == ' ' || c == '\t' || c == '\r' || c == '\n';

  if (state_ == State::WaitBrace) {
    if (space) return;
    if (c != '{') {
      fail(Reply::ExpectedObject);
      return;
    }
    state_ = State::Body;
  }

  if (!inString_ && space) return; // compact as we go

  if (len_ >= CONFIG_JSON_MAX) {
    fail(Reply::TooLarge);
    return;
  }
  json_[len_++] = c;

  if (inString_) {
    if (escaped_) escaped_ = false;
    else if (c == '\\') escaped_ = true;
    else if (c == '"') inString_ = false;
    return;
  }
  if (c == '"') {
    inString_ = true;
  } else if (c == '{' || c == '[') {
    if (depth_ >= CONFIG_MAX_DEPTH) {
      fail(Reply::TooDeep);
      return;
    }
    depth_++;
  } else if ((c == '}' || c == ']') && --depth_ == 0) {
    finish();
  }
}

void ConfigReceiver::forgetStaleLine(uint32_t now) {
  if ((lineLen_ || lineOverflow_) && (uint32_t)(now - lineAt_) >= CONFIG_LINE_IDLE_MS) {
    lineLen_ = 0;
    lineOverflow_ = false;
  }
}

bool ConfigReceiver::handleChar(char c, uint32_t now) {
  if (state_ != State::Idle) {
    receiveChar(c, now);
    return true;
  }
  forgetStaleLine(now);
  lineAt_ = now;
  if (c == '\n' || c == '\r') {
    if (!lineOverflow_ && lineLen_ == 6 && memcmp(line_, "CONFIG", 6) == 0) startReceive(now);
    lineLen_ = 0;
    lineOverflow_ = false;
  } else if (lineLen_ < sizeof line_) {
    line_[lineLen_++] = c;
  } else {
    lineOverflow_ = true;
  }
  return false;
}

void ConfigReceiver::poll(uint32_t now) {
  if (state_ == State::Idle) forgetStaleLine(now);
  if (state_ != State::Idle && (uint32_t)(now - lastByteAt_) >= CONFIG_RX_TIMEOUT_MS) {
    fail(Reply::Timeout);
  }
}
