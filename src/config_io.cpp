#include "config_io.h"

#include <ArduinoJson.h>
#include <EEPROM.h>
#include <string.h>

#include "config.h"
#include "config_json.h"
#include "eeprom_map.h"
#include "watchdog.h"
#include "sequencer_io.h"

constexpr int EEPROM_ADDR = EEPROM_CONFIG_ADDR;
constexpr size_t JSON_BUF_MAX = 1024;       // whitespace-stripped JSON text
constexpr uint32_t RECEIVE_TIMEOUT_MS = 3000; // idle gap that abandons a config in flight

static char json[JSON_BUF_MAX];
static size_t jsonLen = 0;

enum class Rx : uint8_t { Idle, WaitBrace, Body };
static Rx rx = Rx::Idle;
static uint32_t lastByteAt = 0;
static uint8_t depth = 0;
static bool inString = false;
static bool escaped = false;

// While idle, watch for the line "CONFIG".
static char line[8];
static uint8_t lineLen = 0;
static bool lineOverflow = false;

static void saveToEeprom(const SequencerConfig &cfg) {
  uint8_t buf[CONFIG_IMAGE_MAX];
  size_t n = configSerialize(cfg, buf, sizeof buf);
  for (size_t i = 0; i < n; i++) {
    EEPROM.update(EEPROM_ADDR + i, buf[i]); // writes only changed bytes, ~3.3ms each
    watchdogFeed();
  }
}

void configIoBegin() {
  uint8_t buf[CONFIG_IMAGE_MAX];
  for (size_t i = 0; i < sizeof buf; i++) buf[i] = EEPROM.read(EEPROM_ADDR + i);

  SequencerConfig cfg;
  const char *why = "";
  if (configDeserialize(buf, sizeof buf, cfg, why) && sequencerIoApplyConfig(cfg)) {
    Serial.println(F("Config: loaded from EEPROM"));
  } else {
    // Empty or invalid: stay on the built-in defaults rather than refuse to run.
    Serial.print(F("Config: using built-in defaults ("));
    Serial.print(why);
    Serial.println(')');
  }
}

static void reset() {
  rx = Rx::Idle;
  jsonLen = 0;
  depth = 0;
  inString = false;
  escaped = false;
}

static void replyError(const char *reason) {
  Serial.print(F("ERROR: "));
  Serial.println(reason);
  reset();
}

static void replyError(const __FlashStringHelper *reason) {
  Serial.print(F("ERROR: "));
  Serial.println(reason);
  reset();
}

static void finish() {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, json, jsonLen);
  if (e) {
    Serial.print(F("ERROR: invalid JSON: "));
    Serial.println(e.c_str());
    reset();
    return;
  }
  SequencerConfig cfg;
  char err[96];
  if (!configFromJson(doc, cfg, err, sizeof err)) {
    replyError(err);
    return;
  }
  if (!sequencerIoApplyConfig(cfg)) {
    replyError(F("busy, a band is transmitting"));
    return;
  }
  saveToEeprom(cfg);
  Serial.println(F("OK"));
  reset();
}

static void startReceive() {
  if (!sequencerIoApplyConfig(sequencerIoConfig())) { // idle check, no change made
    replyError(F("busy, a band is transmitting"));
    return;
  }
  reset();
  rx = Rx::WaitBrace;
  lastByteAt = millis();
  Serial.println(F("READY"));
}

// One character of the config text. Returns once the object is complete or on error.
static void receiveChar(char c) {
  lastByteAt = millis();
  bool space = c == ' ' || c == '\t' || c == '\r' || c == '\n';

  if (rx == Rx::WaitBrace) {
    if (space) return;
    if (c != '{') {
      replyError(F("expected a JSON object"));
      return;
    }
    rx = Rx::Body;
  }

  if (!inString && space) return; // compact as we go

  if (jsonLen >= JSON_BUF_MAX) {
    replyError(F("config too large"));
    return;
  }
  json[jsonLen++] = c;

  if (inString) {
    if (escaped) escaped = false;
    else if (c == '\\') escaped = true;
    else if (c == '"') inString = false;
    return;
  }
  if (c == '"') inString = true;
  else if (c == '{' || c == '[') depth++;
  else if ((c == '}' || c == ']') && --depth == 0) finish();
}

bool configIoHandleChar(char c) {
  if (rx != Rx::Idle) {
    receiveChar(c);
    return true;
  }
  if (c == '\n' || c == '\r') {
    if (!lineOverflow && lineLen == 6 && memcmp(line, "CONFIG", 6) == 0) startReceive();
    lineLen = 0;
    lineOverflow = false;
  } else if (lineLen < sizeof line) {
    line[lineLen++] = c;
  } else {
    lineOverflow = true;
  }
  return false;
}

void configIoPoll() {
  if (rx != Rx::Idle && (uint32_t)(millis() - lastByteAt) >= RECEIVE_TIMEOUT_MS) {
    replyError(F("timeout waiting for config"));
  }
}
