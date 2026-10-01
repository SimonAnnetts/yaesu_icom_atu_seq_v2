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

// Drive a normal tune: START at t0, KEY asserts at +110, releases at +keyOff.
static void normalTune(uint32_t t0, uint32_t keyOff) {
  TEST_ASSERT_TRUE(d.begin(key, t0));
  run(t0, t0 + 109);
  key = true;
  run(t0 + 110, t0 + keyOff - 1);
  key = false;
}

void test_start_asserted_immediately() {
  TEST_ASSERT_FALSE(d.startAsserted());
  TEST_ASSERT_TRUE(d.begin(false, 0));
  TEST_ASSERT_TRUE(d.startAsserted());
  TEST_ASSERT_TRUE(d.busy());
}

void test_successful_tune() {
  normalTune(1000, 1500);
  run(2500, 2524); // KEY released at 2500; still inside the 25ms confirm window
  TEST_ASSERT_EQUAL(R::None, d.takeResult());
  run(2525, 2540);
  TEST_ASSERT_TRUE(d.startAsserted()); // success holds START for the caller
  TEST_ASSERT_TRUE(d.busy());
  TEST_ASSERT_EQUAL(R::Success, d.takeResult());
  TEST_ASSERT_EQUAL(R::None, d.takeResult()); // delivered once
  d.release(2600);
  TEST_ASSERT_FALSE(d.startAsserted());
  TEST_ASSERT_FALSE(d.busy());
}

void test_release_defers_to_the_minimum_hold() {
  // A very short success: KEY up at +120, back down at +125, confirmed at +150.
  TEST_ASSERT_TRUE(d.begin(false, 0));
  key = true;
  run(110, 119);
  key = false;
  run(120, 149);
  TEST_ASSERT_EQUAL(R::Success, d.takeResult()); // 120 + 25 = 145
  d.release(146);
  TEST_ASSERT_TRUE(d.startAsserted()); // held 146ms < 150
  run(147, 149);
  TEST_ASSERT_TRUE(d.startAsserted());
  run(150, 150);
  TEST_ASSERT_FALSE(d.startAsserted());
}

void test_no_atu_when_key_never_asserts() {
  TEST_ASSERT_TRUE(d.begin(false, 0));
  run(0, 499);
  TEST_ASSERT_TRUE(d.startAsserted());
  TEST_ASSERT_EQUAL(R::None, d.takeResult());
  run(500, 500);
  TEST_ASSERT_EQUAL(R::NoAtu, d.takeResult());
  TEST_ASSERT_FALSE(d.startAsserted()); // released straight away (held 500ms)
  TEST_ASSERT_FALSE(d.busy());
}

void test_busy_timeout() {
  TEST_ASSERT_TRUE(d.begin(false, 0));
  key = true;
  run(110, 110 + AH4_BUSY_TIMEOUT_MS - 1);
  TEST_ASSERT_TRUE(d.startAsserted());
  run(110 + AH4_BUSY_TIMEOUT_MS, 110 + AH4_BUSY_TIMEOUT_MS);
  TEST_ASSERT_EQUAL(R::Timeout, d.takeResult());
  TEST_ASSERT_FALSE(d.startAsserted());
}

void test_bounce_after_release_is_a_failure() {
  TEST_ASSERT_TRUE(d.begin(false, 0));
  key = true;
  run(110, 999);
  key = false;
  run(1000, 1009);
  key = true; // comes back inside the 25ms window
  run(1010, 1011);
  TEST_ASSERT_EQUAL(R::Bounce, d.takeResult());
  TEST_ASSERT_FALSE(d.startAsserted());
}

void test_brief_glitch_inside_confirm_window_still_fails() {
  TEST_ASSERT_TRUE(d.begin(false, 0));
  key = true;
  run(110, 999);
  key = false;
  run(1000, 1005);
  key = true;
  d.poll(key, 1006); // one-sample glitch
  key = false;
  run(1007, 1100);
  TEST_ASSERT_EQUAL(R::Bounce, d.takeResult());
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
  TEST_ASSERT_TRUE(d.startAsserted()); // a short START would toggle the AH-4's bypass
  TEST_ASSERT_TRUE(d.busy());
  run(21, 149);
  TEST_ASSERT_TRUE(d.startAsserted());
  run(150, 150);
  TEST_ASSERT_FALSE(d.startAsserted());
  TEST_ASSERT_FALSE(d.busy());
}

void test_abort_while_busy_releases_at_once() {
  TEST_ASSERT_TRUE(d.begin(false, 0));
  key = true;
  run(110, 999);
  d.abort(1000);
  TEST_ASSERT_FALSE(d.startAsserted());
  TEST_ASSERT_EQUAL(R::Aborted, d.takeResult());
}

void test_abort_while_holding_after_success() {
  normalTune(0, 500);
  run(610, 700);
  TEST_ASSERT_TRUE(d.startAsserted());
  d.abort(701);
  TEST_ASSERT_FALSE(d.startAsserted());
}

void test_abort_when_idle_is_harmless() {
  d.abort(0);
  TEST_ASSERT_FALSE(d.busy());
  TEST_ASSERT_EQUAL(R::None, d.takeResult());
}

void test_begin_rejected_while_running_or_releasing() {
  TEST_ASSERT_TRUE(d.begin(false, 0));
  TEST_ASSERT_FALSE(d.begin(false, 10));
  d.abort(20); // START still held to 150
  TEST_ASSERT_FALSE(d.begin(false, 30));
  run(21, 150);
  TEST_ASSERT_TRUE(d.begin(false, 200));
}

void test_second_cycle_after_success() {
  normalTune(0, 400);
  run(510, 600);
  d.release(601);
  TEST_ASSERT_EQUAL(R::Success, d.takeResult());
  TEST_ASSERT_TRUE(d.begin(false, 700));
  TEST_ASSERT_TRUE(d.startAsserted());
}

void test_millis_wraparound() {
  uint32_t t0 = 0xFFFFFF00u;
  TEST_ASSERT_TRUE(d.begin(false, t0));
  for (uint32_t i = 0; i < 400; i++) d.poll(false, t0 + i);
  TEST_ASSERT_EQUAL(R::None, d.takeResult()); // 400ms: not timed out yet, across the wrap
  for (uint32_t i = 400; i <= 500; i++) d.poll(false, t0 + i);
  TEST_ASSERT_EQUAL(R::NoAtu, d.takeResult());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_start_asserted_immediately);
  RUN_TEST(test_successful_tune);
  RUN_TEST(test_release_defers_to_the_minimum_hold);
  RUN_TEST(test_no_atu_when_key_never_asserts);
  RUN_TEST(test_busy_timeout);
  RUN_TEST(test_bounce_after_release_is_a_failure);
  RUN_TEST(test_brief_glitch_inside_confirm_window_still_fails);
  RUN_TEST(test_key_stuck_refuses_to_start);
  RUN_TEST(test_abort_early_keeps_start_for_the_minimum_hold);
  RUN_TEST(test_abort_while_busy_releases_at_once);
  RUN_TEST(test_abort_while_holding_after_success);
  RUN_TEST(test_abort_when_idle_is_harmless);
  RUN_TEST(test_begin_rejected_while_running_or_releasing);
  RUN_TEST(test_second_cycle_after_success);
  RUN_TEST(test_millis_wraparound);
  return UNITY_END();
}
