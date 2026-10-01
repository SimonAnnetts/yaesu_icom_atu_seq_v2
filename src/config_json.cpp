#include "config_json.h"

#include <stdio.h>
#include <string.h>

#include "config.h"

static const char *const BAND_KEYS[SEQ_BANDS] = {"HF", "50M", "144M", "430M"};
static const char *const STAGE_KEYS[SEQ_STAGES] = {"seq1", "seq2", "seq3"};
static const char *const TIMING_KEYS[SEQ_STAGES] = {"seq1_to_seq2", "seq2_to_seq3", "seq3_to_tx"};

static void fail(char *err, size_t n, const char *fmt, const char *a = "", const char *b = "") {
  snprintf(err, n, fmt, a, b);
}

static int8_t bandIndex(const char *name) {
  for (uint8_t i = 0; i < SEQ_BANDS; i++) {
    if (strcmp(name, BAND_KEYS[i]) == 0) return i;
  }
  return -1;
}

static bool parseBand(JsonVariantConst v, const char *name, BandConfig &out, char *err, size_t n) {
  if (!v.is<JsonObjectConst>()) {
    fail(err, n, "band %s: missing or not an object", name);
    return false;
  }
  JsonVariantConst lo = v["freq_min_hz"], hi = v["freq_max_hz"];
  if (!lo.is<uint32_t>()) {
    fail(err, n, "band %s: freq_min_hz missing or not a number", name);
    return false;
  }
  if (!hi.is<uint32_t>()) {
    fail(err, n, "band %s: freq_max_hz missing or not a number", name);
    return false;
  }
  out.freqMinHz = lo.as<uint32_t>();
  out.freqMaxHz = hi.as<uint32_t>();

  JsonVariantConst timing = v["timing_ms"];
  if (!timing.is<JsonObjectConst>()) {
    fail(err, n, "band %s: timing_ms missing or not an object", name);
    return false;
  }
  for (uint8_t i = 0; i < SEQ_STAGES; i++) {
    JsonVariantConst t = timing[TIMING_KEYS[i]];
    // Anything above uint16 range can't be a legal timing anyway.
    if (!t.is<uint16_t>()) {
      fail(err, n, "band %s: timing_ms.%s missing, negative, or not a number", name, TIMING_KEYS[i]);
      return false;
    }
    out.gapMs[i] = t.as<uint16_t>();
  }

  JsonVariantConst tune = v["tune_profile"];
  if (!tune.is<JsonObjectConst>()) {
    fail(err, n, "band %s: tune_profile missing or not an object", name);
    return false;
  }
  for (uint8_t i = 0; i < SEQ_STAGES; i++) {
    JsonVariantConst f = tune[STAGE_KEYS[i]];
    if (!f.is<bool>()) {
      fail(err, n, "band %s: tune_profile.%s missing or not true/false", name, STAGE_KEYS[i]);
      return false;
    }
    out.tuneProfile[i] = f.as<bool>();
  }
  return true;
}

static bool parseTriggers(JsonVariantConst arr, SequencerConfig &out, char *err, size_t n) {
  if (arr.isNull()) return true; // optional
  if (!arr.is<JsonArrayConst>()) {
    fail(err, n, "cross_band_triggers is not an array");
    return false;
  }
  for (JsonVariantConst item : arr.as<JsonArrayConst>()) {
    if (out.triggerCount >= MAX_TRIGGERS) {
      fail(err, n, "too many cross_band_triggers (max 8)");
      return false;
    }
    const char *src = item["source_band"] | "";
    const char *dst = item["target_band"] | "";
    const char *stage = item["target_output"] | "";
    int8_t s = bandIndex(src), d = bandIndex(dst);
    if (s < 0) {
      fail(err, n, "cross_band_triggers: unknown source_band '%s'", src);
      return false;
    }
    if (d < 0) {
      fail(err, n, "cross_band_triggers: unknown target_band '%s'", dst);
      return false;
    }
    int8_t st = -1;
    for (uint8_t i = 0; i < SEQ_STAGES; i++) {
      if (strcmp(stage, STAGE_KEYS[i]) == 0) st = i;
    }
    if (st < 0) {
      fail(err, n, "cross_band_triggers: target_output '%s' must be seq1, seq2 or seq3", stage);
      return false;
    }
    out.trigger[out.triggerCount++] = {(uint8_t)s, (uint8_t)d, (uint8_t)st};
  }
  return true;
}

bool configFromJson(JsonVariantConst root, SequencerConfig &out, char *err, size_t errLen) {
  if (!root.is<JsonObjectConst>()) {
    fail(err, errLen, "config is not a JSON object");
    return false;
  }
  JsonVariantConst ver = root["schema_version"];
  if (!ver.is<int>() || ver.as<int>() != 1) {
    fail(err, errLen, "unsupported or missing schema_version (need 1)");
    return false;
  }
  JsonVariantConst bands = root["bands"];
  if (!bands.is<JsonObjectConst>()) {
    fail(err, errLen, "bands missing or not an object");
    return false;
  }

  SequencerConfig cfg = {};
  for (uint8_t i = 0; i < SEQ_BANDS; i++) {
    if (!parseBand(bands[BAND_KEYS[i]], BAND_KEYS[i], cfg.band[i], err, errLen)) return false;
  }
  if (!parseTriggers(root["cross_band_triggers"], cfg, err, errLen)) return false;

  const char *why = "";
  if (!configValidate(cfg, why)) {
    fail(err, errLen, "%s", why);
    return false;
  }
  out = cfg;
  return true;
}
