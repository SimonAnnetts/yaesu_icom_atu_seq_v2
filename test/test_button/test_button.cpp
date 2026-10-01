#include <unity.h>

#include "button.h"

using E = Button::Event;

static Button b;

void setUp() {
  b = Button();
  b.init(false, 0);
}

// Feed a raw level every ms from `from` to `to`; return the events seen.
struct Seen {
  int pressed = 0, released = 0;
};
static Seen feed(bool raw, uint32_t from, uint32_t to) {
  Seen s;
  for (uint32_t t = from; t <= to; t++) {
    E e = b.update(raw, t);
    if (e == E::Pressed) s.pressed++;
    if (e == E::Released) s.released++;
  }
  return s;
}

void test_clean_press_and_release() {
  Seen s = feed(true, 100, 129);
  TEST_ASSERT_EQUAL(0, s.pressed); // not yet: 29ms
  TEST_ASSERT_FALSE(b.pressed());
  s = feed(true, 130, 500);
  TEST_ASSERT_EQUAL(1, s.pressed); // accepted once the level has held 30ms
  TEST_ASSERT_TRUE(b.pressed());
  s = feed(false, 501, 600);
  TEST_ASSERT_EQUAL(1, s.released);
  TEST_ASSERT_FALSE(b.pressed());
}

void test_held_ms_measured_from_first_edge() {
  feed(true, 100, 400);
  feed(false, 401, 500);
  // pressed at the 100ms edge, released at the 401ms edge
  TEST_ASSERT_EQUAL(301, b.heldMs());
}

void test_holding_does_not_repeat() {
  Seen s = feed(true, 0, 5000);
  TEST_ASSERT_EQUAL(1, s.pressed);
  TEST_ASSERT_EQUAL(0, s.released);
}

void test_bounce_on_press_gives_one_event() {
  // contacts chatter for ~15ms, then settle
  Seen s;
  const uint32_t edges[] = {100, 103, 106, 109, 112, 115};
  bool level = true;
  uint32_t t = 100;
  for (uint32_t e : edges) {
    for (; t < e; t++) {
      if (b.update(level, t) == E::Pressed) s.pressed++;
    }
    level = !level;
  }
  level = true;
  for (; t < 300; t++) {
    if (b.update(level, t) == E::Pressed) s.pressed++;
  }
  TEST_ASSERT_EQUAL(1, s.pressed);
  TEST_ASSERT_TRUE(b.pressed());
}

void test_short_glitch_ignored() {
  Seen s = feed(true, 100, 120); // 21ms blip
  s.pressed += feed(false, 121, 400).pressed;
  TEST_ASSERT_EQUAL(0, s.pressed);
  TEST_ASSERT_FALSE(b.pressed());
}

void test_bounce_on_release_gives_one_event() {
  feed(true, 0, 300);
  Seen s;
  for (uint32_t t = 301; t < 310; t++) {
    if (b.update(t % 2 == 0, t) == E::Released) s.released++; // chatter
  }
  s.released += feed(false, 310, 500).released;
  TEST_ASSERT_EQUAL(1, s.released);
  TEST_ASSERT_FALSE(b.pressed());
}

void test_held_at_boot_reports_no_press() {
  b.init(true, 0); // button already down when the firmware starts
  Seen s = feed(true, 0, 1000);
  TEST_ASSERT_EQUAL(0, s.pressed);
  TEST_ASSERT_TRUE(b.pressed());
  s = feed(false, 1001, 1100);
  TEST_ASSERT_EQUAL(1, s.released);
}

void test_two_presses() {
  Seen a = feed(true, 0, 200);
  feed(false, 201, 400);
  Seen c = feed(true, 401, 600);
  TEST_ASSERT_EQUAL(1, a.pressed);
  TEST_ASSERT_EQUAL(1, c.pressed);
}

void test_millis_wraparound() {
  uint32_t t0 = 0xFFFFFFF0u;
  b.init(false, t0);
  Seen s;
  for (uint32_t i = 0; i < 100; i++) {
    if (b.update(true, t0 + i) == E::Pressed) s.pressed++;
  }
  TEST_ASSERT_EQUAL(1, s.pressed);
  for (uint32_t i = 100; i < 200; i++) b.update(false, t0 + i);
  TEST_ASSERT_EQUAL(100, b.heldMs());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_clean_press_and_release);
  RUN_TEST(test_held_ms_measured_from_first_edge);
  RUN_TEST(test_holding_does_not_repeat);
  RUN_TEST(test_bounce_on_press_gives_one_event);
  RUN_TEST(test_short_glitch_ignored);
  RUN_TEST(test_bounce_on_release_gives_one_event);
  RUN_TEST(test_held_at_boot_reports_no_press);
  RUN_TEST(test_two_presses);
  RUN_TEST(test_millis_wraparound);
  return UNITY_END();
}
