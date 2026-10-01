#include <unity.h>

#include "blinker.h"

using P = Blinker::Pattern;

static Blinker b;

void setUp() { b = Blinker(); }

static int risingEdges(uint32_t from, uint32_t to) {
  int n = 0;
  bool prev = b.level(from);
  for (uint32_t t = from + 1; t <= to; t++) {
    bool l = b.level(t);
    if (l && !prev) n++;
    prev = l;
  }
  return n;
}

void test_off_and_on() {
  b.set(P::Off, 0);
  TEST_ASSERT_FALSE(b.level(0));
  TEST_ASSERT_FALSE(b.level(123456));
  b.set(P::On, 1000);
  TEST_ASSERT_TRUE(b.level(1000));
  TEST_ASSERT_TRUE(b.level(999999));
  TEST_ASSERT_FALSE(b.finished(999999));
}

void test_success_is_three_blinks_then_dark() {
  b.set(P::Success, 5000);
  TEST_ASSERT_TRUE(b.level(5000)); // starts lit
  TEST_ASSERT_EQUAL(2, risingEdges(5000, 5000 + 899)); // 3 blinks = 1 initial + 2 more
  TEST_ASSERT_FALSE(b.level(5000 + 900));
  TEST_ASSERT_FALSE(b.level(5000 + 5000));
  TEST_ASSERT_FALSE(b.finished(5000 + 899));
  TEST_ASSERT_TRUE(b.finished(5000 + 900));
}

void test_failure_is_fast_blinking_for_about_a_second() {
  b.set(P::Failure, 0);
  TEST_ASSERT_TRUE(b.level(0));
  TEST_ASSERT_EQUAL(4, risingEdges(0, 999)); // 5 blinks
  TEST_ASSERT_FALSE(b.level(1000));
  TEST_ASSERT_FALSE(b.level(1099));
  TEST_ASSERT_TRUE(b.finished(1000));
}

void test_setting_a_pattern_restarts_it() {
  b.set(P::Success, 0);
  TEST_ASSERT_FALSE(b.level(200)); // in the off half of the first blink
  b.set(P::Success, 200);
  TEST_ASSERT_TRUE(b.level(200));
}

void test_millis_wraparound() {
  uint32_t t0 = 0xFFFFFF00u;
  b.set(P::Success, t0);
  TEST_ASSERT_TRUE(b.level(t0));
  TEST_ASSERT_FALSE(b.level(t0 + 200));
  TEST_ASSERT_TRUE(b.level(t0 + 300)); // across the wrap
  TEST_ASSERT_TRUE(b.finished(t0 + 900));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_off_and_on);
  RUN_TEST(test_success_is_three_blinks_then_dark);
  RUN_TEST(test_failure_is_fast_blinking_for_about_a_second);
  RUN_TEST(test_setting_a_pattern_restarts_it);
  RUN_TEST(test_millis_wraparound);
  return UNITY_END();
}
