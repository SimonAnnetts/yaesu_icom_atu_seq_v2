#include <unity.h>

#include "ah4.h"

using R = Ah4Driver::Result;

static Ah4Driver d;
static bool key; // simulated KEY line (true = asserted)

void setUp() {
  d = Ah4Driver();
  key = false;
}

static void run(uint32_t from, uint32_t to) {
  for (uint32_t t = from; t <= to; t++) d.poll(key, t);
}

// Alinco EDX-2 (bench): KEY asserts ~35ms after START is released.
static void edx2Tune(uint32_t t0, uint32_t keyHeldMs) {
  TEST_ASSERT_TRUE(d.begin(key, t0));
  run(t0, t0 + AH4_START_HOLD_MS + 34);
  key = true;
  run(t0 + AH4_START_HOLD_MS + 35, t0 + AH4_START_HOLD_MS + 35 + keyHeldMs - 1);
  key = false;
}

void test_start_asserted_immediately() {
  TEST_ASSERT_FALSE(d.startAsserted());
  TEST_ASSERT_TRUE(d.begin(false, 0));
  TEST_ASSERT_TRUE(d.startAsserted());
  TEST_ASSERT_TRUE(d.busy());
  TEST_ASSERT_FALSE(d.keySeen());
}

void test_start_released_after_the_hold_whatever_key_does() {
  TEST_ASSERT_TRUE(d.begin(false, 1000));
  run(1000, 1000 + AH4_START_HOLD_MS - 1);
  TEST_ASSERT_TRUE(d.startAsserted());
  run(1000 + AH4_START_HOLD_MS, 1000 + AH4_START_HOLD_MS);
  TEST_ASSERT_FALSE(d.startAsserted());
  TEST_ASSERT_TRUE(d.busy()); // still waiting for KEY
}

void test_edx2_key_after_start_release() {
  // The bench log: KEY ~35ms after START is released, however long it was held.
  edx2Tune(0, 1000);
  TEST_ASSERT_TRUE(d.keySeen());
  TEST_ASSERT_FALSE(d.startAsserted()); // already released: KEY came after it
  TEST_ASSERT_EQUAL(R::None, d.takeResult()); // KEY just released, confirming
  run(AH4_START_HOLD_MS + 35 + 1000, AH4_START_HOLD_MS + 35 + 1000 + AH4_CONFIRM_MS);
  TEST_ASSERT_EQUAL(R::Success, d.takeResult());
  TEST_ASSERT_EQUAL(R::None, d.takeResult()); // delivered once
  TEST_ASSERT_FALSE(d.busy());
}

void test_genuine_ah4_key_during_the_hold() {
  // K9EQ: KEY ~300ms after START, while START is still asserted.
  TEST_ASSERT_TRUE(d.begin(false, 0));
  run(0, 299);
  TEST_ASSERT_FALSE(d.keySeen());
  key = true;
  run(300, 300);
  TEST_ASSERT_TRUE(d.keySeen()); // cue to key the radio
  TEST_ASSERT_TRUE(d.startAsserted());
  run(301, 1500);
  TEST_ASSERT_FALSE(d.startAsserted()); // released on schedule at 560, KEY still asserted
  key = false;
  run(1501, 1501 + AH4_CONFIRM_MS);
  TEST_ASSERT_EQUAL(R::Success, d.takeResult());
}

void test_fast_tune_does_not_cut_the_start_hold_short() {
  TEST_ASSERT_TRUE(d.begin(false, 0));
  run(0, 99);
  key = true;
  run(100, 199);
  key = false;
  run(200, 200 + AH4_CONFIRM_MS);
  TEST_ASSERT_EQUAL(R::Success, d.takeResult());
  TEST_ASSERT_TRUE(d.startAsserted()); // success keeps the normal 560ms hold
  run(251, AH4_START_HOLD_MS);
  TEST_ASSERT_FALSE(d.startAsserted());
}

void test_no_atu_when_key_never_asserts() {
  TEST_ASSERT_TRUE(d.begin(false, 0));
  uint32_t deadline = AH4_START_HOLD_MS + AH4_KEY_APPEAR_TIMEOUT_MS;
  run(0, deadline - 1);
  TEST_ASSERT_EQUAL(R::None, d.takeResult());
  run(deadline, deadline);
  TEST_ASSERT_EQUAL(R::NoAtu, d.takeResult());
  TEST_ASSERT_FALSE(d.busy());
}

void test_busy_timeout() {
  TEST_ASSERT_TRUE(d.begin(false, 0));
  key = true;
  run(100, 100 + AH4_BUSY_TIMEOUT_MS - 1);
  TEST_ASSERT_EQUAL(R::None, d.takeResult());
  run(100 + AH4_BUSY_TIMEOUT_MS, 100 + AH4_BUSY_TIMEOUT_MS);
  TEST_ASSERT_EQUAL(R::Timeout, d.takeResult());
  TEST_ASSERT_FALSE(d.startAsserted());
}

void test_ah4_not_tuned_signature_is_detected() {
  // K9EQ figure 5: KEY low, released for 20ms, asserted 200ms, released for good.
  TEST_ASSERT_TRUE(d.begin(false, 0));
  key = true;
  run(300, 1999);
  key = false;
  run(2000, 2019); // the 20ms gap
  TEST_ASSERT_EQUAL(R::None, d.takeResult()); // not decided yet
  key = true;
  run(2020, 2021);
  TEST_ASSERT_EQUAL(R::TuneFailed, d.takeResult());
}

void test_brief_glitch_inside_confirm_window_still_fails() {
  edx2Tune(0, 500);
  uint32_t t = AH4_START_HOLD_MS + 35 + 500;
  run(t, t + 5);
  key = true;
  d.poll(key, t + 6); // one-sample glitch inside the confirm window
  key = false;
  run(t + 7, t + 200);
  TEST_ASSERT_EQUAL(R::TuneFailed, d.takeResult());
}

void test_gap_margin_is_comfortable() {
  TEST_ASSERT_GREATER_OR_EQUAL(2 * 20, AH4_CONFIRM_MS); // not-tuned gap is 20ms
}

void test_key_stuck_refuses_to_start() {
  key = true;
  TEST_ASSERT_TRUE(d.begin(key, 0));
  TEST_ASSERT_FALSE(d.startAsserted()); // never drove START
  TEST_ASSERT_FALSE(d.busy());
  TEST_ASSERT_EQUAL(R::KeyStuck, d.takeResult());
}

void test_abort_early_keeps_start_for_the_minimum_hold() {
  TEST_ASSERT_TRUE(d.begin(false, 0));
  run(0, 19);
  d.abort(20);
  TEST_ASSERT_EQUAL(R::Aborted, d.takeResult());
  TEST_ASSERT_TRUE(d.startAsserted()); // a pulse this short would reset the tuner
  TEST_ASSERT_TRUE(d.busy());
  run(21, AH4_START_MIN_HOLD_MS - 1);
  TEST_ASSERT_TRUE(d.startAsserted());
  run(AH4_START_MIN_HOLD_MS, AH4_START_MIN_HOLD_MS);
  TEST_ASSERT_FALSE(d.startAsserted());
  TEST_ASSERT_FALSE(d.busy());
}

void test_abort_during_hold_after_minimum_releases_at_once() {
  TEST_ASSERT_TRUE(d.begin(false, 0));
  run(0, 299);
  d.abort(300); // well before the normal 560ms release
  TEST_ASSERT_FALSE(d.startAsserted());
  TEST_ASSERT_EQUAL(R::Aborted, d.takeResult());
}

void test_abort_while_busy() {
  TEST_ASSERT_TRUE(d.begin(false, 0));
  run(0, 599);
  key = true;
  run(600, 1000);
  d.abort(1001);
  TEST_ASSERT_EQUAL(R::Aborted, d.takeResult());
  TEST_ASSERT_FALSE(d.busy());
}

void test_abort_after_cycle_cuts_a_lingering_start_hold() {
  TEST_ASSERT_TRUE(d.begin(false, 0));
  key = true;
  run(100, 199);
  key = false;
  run(200, 200 + AH4_CONFIRM_MS);
  TEST_ASSERT_EQUAL(R::Success, d.takeResult());
  TEST_ASSERT_TRUE(d.startAsserted()); // success, START still in its hold
  d.abort(300);
  TEST_ASSERT_FALSE(d.startAsserted());
}

void test_abort_when_idle_is_harmless() {
  d.abort(0);
  TEST_ASSERT_FALSE(d.busy());
  TEST_ASSERT_EQUAL(R::None, d.takeResult());
}

void test_begin_rejected_while_running_or_start_held() {
  TEST_ASSERT_TRUE(d.begin(false, 0));
  TEST_ASSERT_FALSE(d.begin(false, 10));
  d.abort(20); // START still held to 150
  TEST_ASSERT_FALSE(d.begin(false, 30));
  run(21, AH4_START_MIN_HOLD_MS);
  TEST_ASSERT_TRUE(d.begin(false, 200));
}

void test_second_cycle_after_success() {
  edx2Tune(0, 400);
  uint32_t t = AH4_START_HOLD_MS + 35 + 400;
  run(t, t + AH4_CONFIRM_MS);
  TEST_ASSERT_EQUAL(R::Success, d.takeResult());
  TEST_ASSERT_TRUE(d.begin(false, t + 1000));
  TEST_ASSERT_TRUE(d.startAsserted());
  TEST_ASSERT_FALSE(d.keySeen()); // fresh cycle
}

void test_millis_wraparound() {
  uint32_t t0 = 0xFFFFFF00u;
  TEST_ASSERT_TRUE(d.begin(false, t0));
  uint32_t deadline = AH4_START_HOLD_MS + AH4_KEY_APPEAR_TIMEOUT_MS;
  for (uint32_t i = 0; i < deadline; i++) d.poll(false, t0 + i);
  TEST_ASSERT_EQUAL(R::None, d.takeResult()); // not timed out yet, across the wrap
  TEST_ASSERT_FALSE(d.startAsserted());       // START released at 560 across the wrap
  d.poll(false, t0 + deadline);
  TEST_ASSERT_EQUAL(R::NoAtu, d.takeResult());
}

void test_key_released_flag_is_the_unkey_cue() {
  TEST_ASSERT_TRUE(d.begin(false, 0));
  key = true;
  run(100, 400);
  TEST_ASSERT_TRUE(d.keySeen());
  TEST_ASSERT_FALSE(d.keyReleased());
  key = false;
  d.poll(key, 401);
  TEST_ASSERT_TRUE(d.keyReleased()); // at the first release, before the confirm window ends
  TEST_ASSERT_EQUAL(R::None, d.takeResult());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_start_asserted_immediately);
  RUN_TEST(test_start_released_after_the_hold_whatever_key_does);
  RUN_TEST(test_edx2_key_after_start_release);
  RUN_TEST(test_genuine_ah4_key_during_the_hold);
  RUN_TEST(test_fast_tune_does_not_cut_the_start_hold_short);
  RUN_TEST(test_no_atu_when_key_never_asserts);
  RUN_TEST(test_busy_timeout);
  RUN_TEST(test_ah4_not_tuned_signature_is_detected);
  RUN_TEST(test_brief_glitch_inside_confirm_window_still_fails);
  RUN_TEST(test_gap_margin_is_comfortable);
  RUN_TEST(test_key_stuck_refuses_to_start);
  RUN_TEST(test_abort_early_keeps_start_for_the_minimum_hold);
  RUN_TEST(test_abort_during_hold_after_minimum_releases_at_once);
  RUN_TEST(test_abort_while_busy);
  RUN_TEST(test_abort_after_cycle_cuts_a_lingering_start_hold);
  RUN_TEST(test_abort_when_idle_is_harmless);
  RUN_TEST(test_begin_rejected_while_running_or_start_held);
  RUN_TEST(test_second_cycle_after_success);
  RUN_TEST(test_millis_wraparound);
  RUN_TEST(test_key_released_flag_is_the_unkey_cue);
  return UNITY_END();
}
