#include <unity.h>

#include "sequencer.h"

constexpr uint8_t HF = 0, M50 = 1;

static SequencerConfig cfg;
static Sequencer *seq;

static void advance(uint32_t from, uint32_t to) { // poll every ms like a loop would
  for (uint32_t t = from; t <= to; t++) seq->poll(t);
}

static void expectBand(uint8_t b, bool rx, bool s1, bool s2, bool s3, bool tx) {
  SequencerOutputs o = seq->outputs();
  TEST_ASSERT_EQUAL(rx, o.rx[b]);
  TEST_ASSERT_EQUAL(s1, o.seq[b][0]);
  TEST_ASSERT_EQUAL(s2, o.seq[b][1]);
  TEST_ASSERT_EQUAL(s3, o.seq[b][2]);
  TEST_ASSERT_EQUAL(tx, o.tx[b]);
}

void setUp() {
  cfg = DEFAULT_SEQUENCER_CONFIG;
  for (auto &b : cfg.band) {
    b.gapMs[0] = 50; b.gapMs[1] = 60; b.gapMs[2] = 20; // distinct, to catch mix-ups
  }
  delete seq;
  seq = new Sequencer(cfg);
}

void test_idle_outputs() {
  seq->poll(0);
  expectBand(HF, true, false, false, false, false);
  TEST_ASSERT_FALSE(seq->outputs().txInhibit);
}

void test_up_sequence_timing() {
  seq->stby(HF, true, 1000);
  TEST_ASSERT_TRUE(seq->outputs().txInhibit); // inhibit before anything else moves
  seq->poll(1000);
  expectBand(HF, false, true, false, false, false); // SEQ1 immediately, RX off
  advance(1001, 1049);
  expectBand(HF, false, true, false, false, false);
  advance(1050, 1050);
  expectBand(HF, false, true, true, false, false);  // +50: SEQ2
  advance(1051, 1109);
  expectBand(HF, false, true, true, false, false);
  advance(1110, 1110);
  expectBand(HF, false, true, true, true, false);   // +60: SEQ3
  TEST_ASSERT_TRUE(seq->outputs().txInhibit);
  advance(1111, 1129);
  expectBand(HF, false, true, true, true, false);
  TEST_ASSERT_TRUE(seq->outputs().txInhibit);
  advance(1130, 1130);
  expectBand(HF, false, true, true, true, true);    // +20: TX LED
  TEST_ASSERT_FALSE(seq->outputs().txInhibit);      // released
}

void test_down_sequence_mirrors_up() {
  seq->stby(HF, true, 0);
  advance(0, 500);
  seq->stby(HF, false, 1000);
  seq->poll(1000);
  expectBand(HF, false, true, true, true, false); // TX off immediately
  advance(1001, 1019);
  expectBand(HF, false, true, true, true, false);
  advance(1020, 1020);
  expectBand(HF, false, true, true, false, false); // +20: SEQ3 off
  advance(1021, 1079);
  expectBand(HF, false, true, true, false, false);
  advance(1080, 1080);
  expectBand(HF, false, true, false, false, false); // +60: SEQ2 off
  advance(1081, 1129);
  expectBand(HF, false, true, false, false, false);
  advance(1130, 1130);
  expectBand(HF, true, false, false, false, false); // +50: SEQ1 off, RX on
}

void test_bands_independent_and_inhibit_shared() {
  seq->stby(M50, true, 0);
  advance(0, 500);
  expectBand(HF, true, false, false, false, false);
  expectBand(M50, false, true, true, true, true);
  seq->stby(HF, true, 600);
  TEST_ASSERT_TRUE(seq->outputs().txInhibit); // HF mid-up holds the shared line
  advance(600, 800);
  TEST_ASSERT_FALSE(seq->outputs().txInhibit);
}

void test_stby_idempotent() {
  seq->stby(HF, true, 0);
  advance(0, 80);
  seq->stby(HF, true, 81); // repeat must not restart
  advance(81, 130);
  expectBand(HF, false, true, true, true, true); // same timeline as an uninterrupted up
}

void test_abort_mid_up() {
  seq->stby(HF, true, 0);
  advance(0, 55); // SEQ1, SEQ2 on
  expectBand(HF, false, true, true, false, false);
  seq->stby(HF, false, 56);
  TEST_ASSERT_FALSE(seq->outputs().txInhibit); // released straight away
  seq->poll(56);
  expectBand(HF, false, true, true, false, false);
  advance(57, 115);
  expectBand(HF, false, true, true, false, false);
  advance(116, 116);
  expectBand(HF, false, true, false, false, false); // SEQ2 off one gap (60) after abort
  advance(116, 200);
  expectBand(HF, true, false, false, false, false);
}

void test_reassert_mid_down() {
  seq->stby(HF, true, 0);
  advance(0, 200);
  seq->stby(HF, false, 300);
  advance(300, 325); // SEQ3 off at +20
  expectBand(HF, false, true, true, false, false);
  seq->stby(HF, true, 326); // PTT again before SEQ2 drops
  TEST_ASSERT_TRUE(seq->outputs().txInhibit);
  advance(326, 330);
  expectBand(HF, false, true, true, false, false); // nothing glitched off
  advance(331, 600);
  expectBand(HF, false, true, true, true, true);
  TEST_ASSERT_FALSE(seq->outputs().txInhibit);
}

void test_tune_profile_skips_stage() {
  // HF tune profile skips SEQ3 (defaults copied into cfg by setUp).
  seq->request(HF, true, Profile::Tune, 0);
  advance(0, 55);
  expectBand(HF, false, true, true, false, false);
  TEST_ASSERT_TRUE(seq->outputs().txInhibit);
  advance(56, 115); // SEQ2 gap is 60 -> settles at +50+60
  expectBand(HF, false, true, true, false, true);
  TEST_ASSERT_FALSE(seq->outputs().txInhibit);
  seq->request(HF, false, Profile::Tune, 200);
  advance(200, 400);
  expectBand(HF, true, false, false, false, false);
}

void test_hold_suppresses_stby_only() {
  seq->setHold(HF, true);
  seq->stby(HF, true, 0);
  advance(0, 500);
  expectBand(HF, true, false, false, false, false); // ignored
  seq->request(HF, true, Profile::Tune, 600);        // tune cycle still works
  advance(600, 900);
  TEST_ASSERT_TRUE(seq->active(HF));
  seq->stby(HF, false, 1000);                        // ignored while held
  advance(1000, 1300);
  TEST_ASSERT_TRUE(seq->active(HF));
  seq->setHold(HF, false);
  seq->stby(HF, false, 1400);
  advance(1400, 1700);
  TEST_ASSERT_TRUE(seq->idle(HF));
}

void test_millis_wraparound() {
  uint32_t t0 = 0xFFFFFFF0u;
  seq->stby(HF, true, t0);
  for (uint32_t i = 0; i < 300; i++) seq->poll(t0 + i);
  expectBand(HF, false, true, true, true, true);
}

void test_default_config_has_no_triggers() {
  TEST_ASSERT_EQUAL(0, DEFAULT_SEQUENCER_CONFIG.triggerCount);
}

// 50M -> 144M SEQ1, as in config/sequencer.json
static void addPreampRule() {
  cfg.triggerCount = 1;
  cfg.trigger[0] = {M50, 2, 0};
}

void test_trigger_follows_source_seq1() {
  addPreampRule();
  seq->stby(M50, true, 0);
  seq->poll(0);
  TEST_ASSERT_TRUE(seq->outputs().seq[2][0]); // 144M SEQ1 snaps on with 50M SEQ1
  expectBand(2, true, true, false, false, false); // ...and only that output; its RX stays lit
  advance(1, 300);
  expectBand(2, true, true, false, false, false); // 50M's later stages don't spread
  seq->stby(M50, false, 400);
  advance(400, 700); // 50M down-sequence: SEQ1 drops last
  expectBand(2, true, false, false, false, false);
  TEST_ASSERT_TRUE(seq->idle(M50));
}

void test_trigger_holds_until_source_seq1_off() {
  addPreampRule();
  seq->stby(M50, true, 0);
  advance(0, 300);
  seq->stby(M50, false, 400);
  advance(400, 419); // TX off, SEQ3 not yet down
  TEST_ASSERT_TRUE(seq->outputs().seq[2][0]);
  advance(420, 600); // SEQ3 (20), SEQ2 (60), then SEQ1 (50) at +130
  TEST_ASSERT_FALSE(seq->outputs().seq[2][0]);
}

void test_trigger_ors_with_target_own_sequence() {
  addPreampRule();
  seq->stby(2, true, 0); // 144M transmitting itself
  advance(0, 300);
  seq->stby(M50, true, 301);
  seq->poll(301);
  seq->stby(M50, false, 302);
  advance(302, 800);
  // 50M finished: the rule must not have switched off 144M's own SEQ1
  expectBand(2, false, true, true, true, true);
}

void test_two_rules_same_output() {
  cfg.triggerCount = 2;
  cfg.trigger[0] = {M50, 2, 0};
  cfg.trigger[1] = {HF, 2, 0};
  seq->stby(M50, true, 0);
  seq->stby(HF, true, 0);
  seq->poll(0);
  TEST_ASSERT_TRUE(seq->outputs().seq[2][0]);
  seq->stby(M50, false, 10);
  advance(10, 300); // 50M finishes, HF still up
  TEST_ASSERT_TRUE(seq->idle(M50));
  TEST_ASSERT_TRUE(seq->outputs().seq[2][0]);
}

void test_all_idle() {
  TEST_ASSERT_TRUE(seq->allIdle());
  seq->stby(HF, true, 0);
  TEST_ASSERT_FALSE(seq->allIdle());
}

void test_band_for_frequency() {
  const SequencerConfig &c = DEFAULT_SEQUENCER_CONFIG;
  TEST_ASSERT_EQUAL(0, bandForFrequency(c, 14250000));
  TEST_ASSERT_EQUAL(0, bandForFrequency(c, 1800000));   // edges are inclusive
  TEST_ASSERT_EQUAL(0, bandForFrequency(c, 29700000));
  TEST_ASSERT_EQUAL(1, bandForFrequency(c, 50150000));
  TEST_ASSERT_EQUAL(2, bandForFrequency(c, 145500000));
  TEST_ASSERT_EQUAL(3, bandForFrequency(c, 433500000));
  TEST_ASSERT_EQUAL(-1, bandForFrequency(c, 1799990));
  TEST_ASSERT_EQUAL(-1, bandForFrequency(c, 100000000)); // between 50M and 144M
  TEST_ASSERT_EQUAL(-1, bandForFrequency(c, 1296000000));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_idle_outputs);
  RUN_TEST(test_up_sequence_timing);
  RUN_TEST(test_down_sequence_mirrors_up);
  RUN_TEST(test_bands_independent_and_inhibit_shared);
  RUN_TEST(test_stby_idempotent);
  RUN_TEST(test_abort_mid_up);
  RUN_TEST(test_reassert_mid_down);
  RUN_TEST(test_tune_profile_skips_stage);
  RUN_TEST(test_hold_suppresses_stby_only);
  RUN_TEST(test_millis_wraparound);
  RUN_TEST(test_default_config_has_no_triggers);
  RUN_TEST(test_trigger_follows_source_seq1);
  RUN_TEST(test_trigger_holds_until_source_seq1_off);
  RUN_TEST(test_trigger_ors_with_target_own_sequence);
  RUN_TEST(test_two_rules_same_output);
  RUN_TEST(test_all_idle);
  RUN_TEST(test_band_for_frequency);
  return UNITY_END();
}
