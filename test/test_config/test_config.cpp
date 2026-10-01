#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unity.h>

#include "config.h"
#include "config_json.h"

static std::string readFile(const char *path) {
  FILE *f = fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string s;
  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

static char err[96];

// Parse JSON text; true if accepted. Rejections leave the reason in err.
static bool load(const std::string &text, SequencerConfig &cfg) {
  JsonDocument doc;
  if (deserializeJson(doc, text)) {
    snprintf(err, sizeof err, "not json");
    return false;
  }
  return configFromJson(doc, cfg, err, sizeof err);
}

// The shipped example, parsed once, then edited per test.
static JsonDocument goodDoc() {
  JsonDocument doc;
  TEST_ASSERT_EQUAL(DeserializationError::Ok,
                    deserializeJson(doc, readFile("config/sequencer.json")).code());
  return doc;
}

static bool loadDoc(JsonDocument &doc, SequencerConfig &cfg) {
  return configFromJson(doc, cfg, err, sizeof err);
}

void setUp() { err[0] = 0; }

// --- the shipped file ---

void test_example_file_parses() {
  SequencerConfig cfg;
  TEST_ASSERT_TRUE_MESSAGE(load(readFile("config/sequencer.json"), cfg), err);
  TEST_ASSERT_EQUAL_UINT32(1800000, cfg.band[0].freqMinHz);
  TEST_ASSERT_EQUAL(50, cfg.band[0].gapMs[0]);
  TEST_ASSERT_EQUAL(20, cfg.band[0].gapMs[2]);
  TEST_ASSERT_FALSE(cfg.band[0].tuneProfile[2]); // HF skips SEQ3 when tuning
  TEST_ASSERT_TRUE(cfg.band[1].tuneProfile[2]);
  TEST_ASSERT_EQUAL(1, cfg.triggerCount);
  TEST_ASSERT_EQUAL(1, cfg.trigger[0].sourceBand); // 50M
  TEST_ASSERT_EQUAL(2, cfg.trigger[0].targetBand); // 144M
  TEST_ASSERT_EQUAL(0, cfg.trigger[0].targetStage); // seq1
}

void test_bench_file_parses() {
  SequencerConfig cfg;
  TEST_ASSERT_TRUE_MESSAGE(load(readFile("config/sequencer-bench.json"), cfg), err);
  // Meant for bench visibility: every step the same, and slow enough to watch.
  uint16_t first = cfg.band[0].gapMs[0];
  TEST_ASSERT_GREATER_OR_EQUAL(300, first);
  for (const BandConfig &b : cfg.band) {
    for (uint16_t g : b.gapMs) TEST_ASSERT_EQUAL(first, g);
  }
}

// The built-in fallback must match config/sequencer.json (apart from triggers).
void test_defaults_match_example_file() {
  SequencerConfig cfg;
  TEST_ASSERT_TRUE_MESSAGE(load(readFile("config/sequencer.json"), cfg), err);
  for (uint8_t b = 0; b < SEQ_BANDS; b++) {
    const BandConfig &d = DEFAULT_SEQUENCER_CONFIG.band[b], &f = cfg.band[b];
    TEST_ASSERT_EQUAL_UINT32(f.freqMinHz, d.freqMinHz);
    TEST_ASSERT_EQUAL_UINT32(f.freqMaxHz, d.freqMaxHz);
    TEST_ASSERT_EQUAL(f.atu, d.atu);
    for (uint8_t i = 0; i < SEQ_STAGES; i++) {
      TEST_ASSERT_EQUAL(f.gapMs[i], d.gapMs[i]);
      TEST_ASSERT_EQUAL(f.tuneProfile[i], d.tuneProfile[i]);
    }
  }
  const char *why;
  TEST_ASSERT_TRUE(configValidate(DEFAULT_SEQUENCER_CONFIG, why));
}

// --- rejections ---

void test_rejects_non_object_and_junk() {
  SequencerConfig cfg;
  TEST_ASSERT_FALSE(load("[1,2,3]", cfg));
  TEST_ASSERT_FALSE(load("42", cfg));
  TEST_ASSERT_FALSE(load("{\"schema_version\": 1", cfg)); // truncated
  TEST_ASSERT_FALSE(load("{}", cfg));
}

void test_rejects_wrong_schema_version() {
  SequencerConfig cfg;
  JsonDocument doc = goodDoc();
  doc["schema_version"] = 2;
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
  TEST_ASSERT_NOT_NULL(strstr(err, "schema_version"));
  doc["schema_version"] = "1";
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
  doc.remove("schema_version");
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
}

void test_rejects_missing_band() {
  SequencerConfig cfg;
  JsonDocument doc = goodDoc();
  doc["bands"].remove("144M");
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
  TEST_ASSERT_NOT_NULL(strstr(err, "144M"));
}

void test_rejects_missing_fields() {
  SequencerConfig cfg;
  JsonDocument doc = goodDoc();
  doc["bands"]["HF"].remove("freq_max_hz");
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
  TEST_ASSERT_NOT_NULL(strstr(err, "freq_max_hz"));

  doc = goodDoc();
  doc["bands"]["50M"]["timing_ms"].remove("seq2_to_seq3");
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
  TEST_ASSERT_NOT_NULL(strstr(err, "seq2_to_seq3"));

  doc = goodDoc();
  doc["bands"]["430M"].remove("tune_profile");
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
  TEST_ASSERT_NOT_NULL(strstr(err, "tune_profile"));

  doc = goodDoc();
  doc["bands"]["HF"]["tune_profile"].remove("seq3");
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
}

void test_rejects_bad_types() {
  SequencerConfig cfg;
  JsonDocument doc = goodDoc();
  doc["bands"]["HF"]["freq_min_hz"] = "1800000";
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));

  doc = goodDoc();
  doc["bands"]["HF"]["timing_ms"]["seq1_to_seq2"] = "fast";
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));

  doc = goodDoc();
  doc["bands"]["HF"]["timing_ms"]["seq1_to_seq2"] = -5;
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));

  doc = goodDoc();
  doc["bands"]["HF"]["tune_profile"]["seq1"] = 1;
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));

  doc = goodDoc();
  doc["bands"] = "nope";
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
}

void test_rejects_inverted_equal_and_overlapping_edges() {
  SequencerConfig cfg;
  JsonDocument doc = goodDoc();
  doc["bands"]["HF"]["freq_min_hz"] = 29700000;
  doc["bands"]["HF"]["freq_max_hz"] = 1800000;
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
  TEST_ASSERT_NOT_NULL(strstr(err, "freq_min_hz"));

  doc = goodDoc();
  doc["bands"]["HF"]["freq_max_hz"] = doc["bands"]["HF"]["freq_min_hz"];
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));

  doc = goodDoc();
  doc["bands"]["HF"]["freq_max_hz"] = 50000000; // touches 50M's lower edge
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
  TEST_ASSERT_NOT_NULL(strstr(err, "overlap"));
}

void test_rejects_oversized_timing() {
  SequencerConfig cfg;
  JsonDocument doc = goodDoc();
  doc["bands"]["HF"]["timing_ms"]["seq3_to_tx"] = 5001;
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
  doc["bands"]["HF"]["timing_ms"]["seq3_to_tx"] = 5000;
  TEST_ASSERT_TRUE_MESSAGE(loadDoc(doc, cfg), err);
  doc["bands"]["HF"]["timing_ms"]["seq3_to_tx"] = 70000; // beyond uint16
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
}

void test_trigger_rules() {
  SequencerConfig cfg;
  JsonDocument doc = goodDoc();
  doc["cross_band_triggers"][0]["source_band"] = "6M";
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
  TEST_ASSERT_NOT_NULL(strstr(err, "source_band"));

  doc = goodDoc();
  doc["cross_band_triggers"][0]["target_output"] = "tx";
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
  TEST_ASSERT_NOT_NULL(strstr(err, "target_output"));

  doc = goodDoc();
  doc["cross_band_triggers"][0]["target_band"] = "50M"; // same as source
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));

  doc = goodDoc();
  doc["cross_band_triggers"] = "x";
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));

  doc = goodDoc();
  doc["cross_band_triggers"][0].remove("target_band");
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
}

void test_atu_flag_parsed_and_defaults() {
  SequencerConfig cfg;
  JsonDocument doc = goodDoc();
  TEST_ASSERT_TRUE_MESSAGE(loadDoc(doc, cfg), err);
  TEST_ASSERT_TRUE(cfg.band[0].atu);   // the shipped example documents the field
  TEST_ASSERT_TRUE(cfg.band[1].atu);
  TEST_ASSERT_FALSE(cfg.band[2].atu);
  TEST_ASSERT_FALSE(cfg.band[3].atu);

  doc["bands"]["50M"]["atu"] = false;  // an EDX-2 owner
  doc["bands"]["144M"]["atu"] = true;
  TEST_ASSERT_TRUE_MESSAGE(loadDoc(doc, cfg), err);
  TEST_ASSERT_FALSE(cfg.band[1].atu);
  TEST_ASSERT_TRUE(cfg.band[2].atu);

  // absent: the previous behaviour (HF and 50M on, VHF/UHF off)
  for (const char *b : {"HF", "50M", "144M", "430M"}) doc["bands"][b].remove("atu");
  TEST_ASSERT_TRUE_MESSAGE(loadDoc(doc, cfg), err);
  TEST_ASSERT_TRUE(cfg.band[0].atu);
  TEST_ASSERT_TRUE(cfg.band[1].atu);
  TEST_ASSERT_FALSE(cfg.band[2].atu);
  TEST_ASSERT_FALSE(cfg.band[3].atu);

  doc["bands"]["HF"]["atu"] = "yes"; // present but not a bool
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
  TEST_ASSERT_NOT_NULL(strstr(err, "atu"));
  doc["bands"]["HF"]["atu"] = 1;
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
}

void test_bench_file_has_the_edx2_setup() {
  SequencerConfig cfg;
  TEST_ASSERT_TRUE_MESSAGE(load(readFile("config/sequencer-bench.json"), cfg), err);
  TEST_ASSERT_TRUE(cfg.band[0].atu);
  TEST_ASSERT_FALSE(cfg.band[1].atu); // the EDX-2 can't tune 50MHz
}

void test_trigger_count_limit_and_omission() {
  SequencerConfig cfg;
  JsonDocument doc = goodDoc();
  doc.remove("cross_band_triggers");
  TEST_ASSERT_TRUE_MESSAGE(loadDoc(doc, cfg), err);
  TEST_ASSERT_EQUAL(0, cfg.triggerCount);

  doc = goodDoc();
  for (int i = 0; i < MAX_TRIGGERS; i++) {
    doc["cross_band_triggers"][i]["source_band"] = "HF";
    doc["cross_band_triggers"][i]["target_band"] = "144M";
    doc["cross_band_triggers"][i]["target_output"] = "seq2";
  }
  TEST_ASSERT_TRUE_MESSAGE(loadDoc(doc, cfg), err);
  TEST_ASSERT_EQUAL(MAX_TRIGGERS, cfg.triggerCount);
  doc["cross_band_triggers"][MAX_TRIGGERS]["source_band"] = "HF";
  doc["cross_band_triggers"][MAX_TRIGGERS]["target_band"] = "144M";
  doc["cross_band_triggers"][MAX_TRIGGERS]["target_output"] = "seq3";
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
}

void test_failed_load_leaves_output_untouched() {
  SequencerConfig cfg = DEFAULT_SEQUENCER_CONFIG;
  JsonDocument doc = goodDoc();
  doc["bands"]["HF"]["timing_ms"]["seq1_to_seq2"] = 9999;
  TEST_ASSERT_FALSE(loadDoc(doc, cfg));
  TEST_ASSERT_EQUAL(50, cfg.band[0].gapMs[0]);
  TEST_ASSERT_EQUAL(0, cfg.triggerCount);
}

// --- EEPROM image ---

static SequencerConfig sampleConfig() {
  SequencerConfig cfg;
  TEST_ASSERT_TRUE_MESSAGE(load(readFile("config/sequencer.json"), cfg), err);
  cfg.band[2].gapMs[1] = 1234;
  return cfg;
}

void test_image_roundtrip() {
  SequencerConfig in = sampleConfig(), out = {};
  uint8_t buf[CONFIG_IMAGE_MAX];
  size_t n = configSerialize(in, buf, sizeof buf);
  TEST_ASSERT_GREATER_THAN(0, n);
  const char *why;
  TEST_ASSERT_TRUE_MESSAGE(configDeserialize(buf, n, out, why), why);
  TEST_ASSERT_EQUAL(1234, out.band[2].gapMs[1]);
  TEST_ASSERT_EQUAL(1, out.triggerCount);
  TEST_ASSERT_EQUAL(2, out.trigger[0].targetBand);
  for (uint8_t b = 0; b < SEQ_BANDS; b++) {
    TEST_ASSERT_EQUAL_UINT32(in.band[b].freqMinHz, out.band[b].freqMinHz);
    TEST_ASSERT_EQUAL_UINT32(in.band[b].freqMaxHz, out.band[b].freqMaxHz);
    for (uint8_t i = 0; i < SEQ_STAGES; i++) {
      TEST_ASSERT_EQUAL(in.band[b].gapMs[i], out.band[b].gapMs[i]);
      TEST_ASSERT_EQUAL(in.band[b].tuneProfile[i], out.band[b].tuneProfile[i]);
    }
  }
}

void test_atu_flags_survive_the_eeprom_image() {
  SequencerConfig in = sampleConfig(), out = {};
  in.band[0].atu = false;
  in.band[1].atu = true;
  in.band[2].atu = true;
  in.band[3].atu = false;
  uint8_t buf[CONFIG_IMAGE_MAX];
  size_t n = configSerialize(in, buf, sizeof buf);
  const char *why;
  TEST_ASSERT_TRUE_MESSAGE(configDeserialize(buf, n, out, why), why);
  for (uint8_t b = 0; b < SEQ_BANDS; b++) TEST_ASSERT_EQUAL(in.band[b].atu, out.band[b].atu);
}

// An image saved before the flag existed: same header, payload without the 4 flag
// bytes. It must still load, with the old behaviour (HF and 50M on).
static size_t stripFlags(uint8_t *buf, size_t n, size_t keepOfFlags) {
  size_t payload = buf[5] | (size_t)buf[6] << 8;
  size_t newPayload = payload - SEQ_BANDS + keepOfFlags;
  buf[5] = newPayload & 0xFF;
  buf[6] = newPayload >> 8;
  uint16_t crc = configCrc16(buf, 7 + newPayload);
  buf[7 + newPayload] = crc & 0xFF;
  buf[7 + newPayload + 1] = crc >> 8;
  (void)n;
  return 7 + newPayload + 2;
}

void test_old_eeprom_image_without_atu_flags_still_loads() {
  SequencerConfig in = sampleConfig(), out = {};
  in.band[1].atu = false; // would be lost: old images can't carry it
  uint8_t buf[CONFIG_IMAGE_MAX];
  size_t n = configSerialize(in, buf, sizeof buf);
  size_t oldN = stripFlags(buf, n, 0);
  const char *why;
  TEST_ASSERT_TRUE_MESSAGE(configDeserialize(buf, oldN, out, why), why);
  TEST_ASSERT_TRUE(out.band[0].atu);
  TEST_ASSERT_TRUE(out.band[1].atu);
  TEST_ASSERT_FALSE(out.band[2].atu);
  TEST_ASSERT_FALSE(out.band[3].atu);
  TEST_ASSERT_EQUAL(1, out.triggerCount); // everything else intact
}

void test_partial_atu_flags_are_rejected() {
  SequencerConfig in = sampleConfig(), out;
  uint8_t buf[CONFIG_IMAGE_MAX];
  size_t n = configSerialize(in, buf, sizeof buf);
  for (size_t keep = 1; keep < SEQ_BANDS; keep++) {
    uint8_t copy[CONFIG_IMAGE_MAX];
    memcpy(copy, buf, n);
    size_t m = stripFlags(copy, n, keep);
    const char *why;
    TEST_ASSERT_FALSE(configDeserialize(copy, m, out, why));
  }
}

void test_image_fits_with_max_triggers() {
  SequencerConfig cfg = sampleConfig();
  cfg.triggerCount = MAX_TRIGGERS;
  for (auto &t : cfg.trigger) t = {0, 1, 0};
  uint8_t buf[CONFIG_IMAGE_MAX];
  TEST_ASSERT_GREATER_THAN(0, configSerialize(cfg, buf, sizeof buf));
  uint8_t tiny[10];
  TEST_ASSERT_EQUAL(0, configSerialize(cfg, tiny, sizeof tiny));
}

void test_any_corrupted_byte_is_rejected() {
  SequencerConfig in = sampleConfig(), out;
  uint8_t buf[CONFIG_IMAGE_MAX];
  size_t n = configSerialize(in, buf, sizeof buf);
  const char *why;
  for (size_t i = 0; i < n; i++) {
    for (uint8_t flip : {0x01, 0x80, 0xFF}) {
      uint8_t saved = buf[i];
      buf[i] ^= flip;
      TEST_ASSERT_FALSE_MESSAGE(configDeserialize(buf, n, out, why), "corruption accepted");
      buf[i] = saved;
    }
  }
}

void test_blank_eeprom_and_short_images_rejected() {
  uint8_t blank[CONFIG_IMAGE_MAX];
  memset(blank, 0xFF, sizeof blank); // erased EEPROM
  SequencerConfig out;
  const char *why;
  TEST_ASSERT_FALSE(configDeserialize(blank, sizeof blank, out, why));
  memset(blank, 0x00, sizeof blank);
  TEST_ASSERT_FALSE(configDeserialize(blank, sizeof blank, out, why));
  TEST_ASSERT_FALSE(configDeserialize(blank, 3, out, why));
  TEST_ASSERT_FALSE(configDeserialize(blank, 0, out, why));
}

void test_truncated_image_rejected() {
  SequencerConfig in = sampleConfig(), out;
  uint8_t buf[CONFIG_IMAGE_MAX];
  size_t n = configSerialize(in, buf, sizeof buf);
  const char *why;
  for (size_t cut = 0; cut < n; cut++) {
    TEST_ASSERT_FALSE(configDeserialize(buf, cut, out, why));
  }
}

void test_valid_crc_but_invalid_config_rejected() {
  SequencerConfig bad = sampleConfig(), out;
  bad.band[0].freqMinHz = bad.band[0].freqMaxHz + 1; // inverted: serialises fine
  uint8_t buf[CONFIG_IMAGE_MAX];
  size_t n = configSerialize(bad, buf, sizeof buf);
  const char *why = "";
  TEST_ASSERT_FALSE(configDeserialize(buf, n, out, why));
  TEST_ASSERT_NOT_NULL(strstr(why, "freq_min_hz"));
}

void test_crc_known_value() {
  const uint8_t s[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQUAL_HEX16(0x29B1, configCrc16(s, sizeof s)); // CRC-16/CCITT-FALSE check value
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_example_file_parses);
  RUN_TEST(test_bench_file_parses);
  RUN_TEST(test_defaults_match_example_file);
  RUN_TEST(test_rejects_non_object_and_junk);
  RUN_TEST(test_rejects_wrong_schema_version);
  RUN_TEST(test_rejects_missing_band);
  RUN_TEST(test_rejects_missing_fields);
  RUN_TEST(test_rejects_bad_types);
  RUN_TEST(test_rejects_inverted_equal_and_overlapping_edges);
  RUN_TEST(test_rejects_oversized_timing);
  RUN_TEST(test_trigger_rules);
  RUN_TEST(test_atu_flag_parsed_and_defaults);
  RUN_TEST(test_bench_file_has_the_edx2_setup);
  RUN_TEST(test_trigger_count_limit_and_omission);
  RUN_TEST(test_failed_load_leaves_output_untouched);
  RUN_TEST(test_image_roundtrip);
  RUN_TEST(test_atu_flags_survive_the_eeprom_image);
  RUN_TEST(test_old_eeprom_image_without_atu_flags_still_loads);
  RUN_TEST(test_partial_atu_flags_are_rejected);
  RUN_TEST(test_image_fits_with_max_triggers);
  RUN_TEST(test_any_corrupted_byte_is_rejected);
  RUN_TEST(test_blank_eeprom_and_short_images_rejected);
  RUN_TEST(test_truncated_image_rejected);
  RUN_TEST(test_valid_crc_but_invalid_config_rejected);
  RUN_TEST(test_crc_known_value);
  return UNITY_END();
}
