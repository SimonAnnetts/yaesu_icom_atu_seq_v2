#include <string.h>
#include <unity.h>

#include "cat_intercept.h"

static CatSnapshot snap() {
  CatSnapshot s;
  s.haveFreqMode = true;
  const uint8_t fm[5] = {0x01, 0x42, 0x89, 0x08, 0x01}; // 14.28908MHz, USB
  memcpy(s.freqMode, fm, 5);
  s.haveTx = true;
  s.tx = 0x80;
  s.haveRx = true;
  s.rx = 0x1F;
  return s;
}

static InterceptDecision decide(const CatSnapshot &s, uint8_t op, uint8_t p0 = 0) {
  const uint8_t cmd[5] = {p0, 0, 0, 0, op};
  return catIntercept(s, cmd);
}

void setUp() {}

void test_freq_mode_query_gets_the_original_mode() {
  InterceptDecision d = decide(snap(), 0x03);
  TEST_ASSERT_EQUAL((int)InterceptAction::Reply, (int)d.action);
  TEST_ASSERT_EQUAL(5, d.replyLen);
  const uint8_t want[5] = {0x01, 0x42, 0x89, 0x08, 0x01};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want, d.reply, 5); // frequency intact, mode USB - not AM
}

void test_status_queries_get_the_snapshot() {
  InterceptDecision tx = decide(snap(), 0xF7);
  TEST_ASSERT_EQUAL((int)InterceptAction::Reply, (int)tx.action);
  TEST_ASSERT_EQUAL(1, tx.replyLen);
  TEST_ASSERT_EQUAL(0x80, tx.reply[0]); // "not transmitting"
  InterceptDecision rx = decide(snap(), 0xE7);
  TEST_ASSERT_EQUAL((int)InterceptAction::Reply, (int)rx.action);
  TEST_ASSERT_EQUAL(0x1F, rx.reply[0]);
}

void test_ptt_commands_are_swallowed() {
  TEST_ASSERT_EQUAL((int)InterceptAction::Swallow, (int)decide(snap(), 0x08).action);
  TEST_ASSERT_EQUAL((int)InterceptAction::Swallow, (int)decide(snap(), 0x88).action);
}

void test_other_commands_are_queued_for_after_the_tune() {
  const uint8_t ops[] = {0x07, 0x17, 0x27, 0x01, 0x11, 0x00, 0x80, 0x4E, 0x8E, 0x0A, 0x0B,
                         0x0C, 0x09, 0xF9, 0x13, 0x23}; // set mode/freq/CAT/..., sat queries
  for (uint8_t op : ops) {
    InterceptDecision d = decide(snap(), op, 0x04);
    TEST_ASSERT_EQUAL_MESSAGE((int)InterceptAction::Queue, (int)d.action, "should be queued");
    TEST_ASSERT_EQUAL(0, d.replyLen);
  }
  TEST_ASSERT_EQUAL((int)InterceptAction::Queue, (int)decide(snap(), 0x5A).action); // unknown opcode
}

void test_queries_without_a_snapshot_field_are_queued_not_invented() {
  CatSnapshot empty;
  TEST_ASSERT_EQUAL((int)InterceptAction::Queue, (int)decide(empty, 0x03).action);
  TEST_ASSERT_EQUAL((int)InterceptAction::Queue, (int)decide(empty, 0xF7).action);
  TEST_ASSERT_EQUAL((int)InterceptAction::Queue, (int)decide(empty, 0xE7).action);
  CatSnapshot partial = snap();
  partial.haveRx = false;
  TEST_ASSERT_EQUAL((int)InterceptAction::Queue, (int)decide(partial, 0xE7).action);
  TEST_ASSERT_EQUAL((int)InterceptAction::Reply, (int)decide(partial, 0x03).action);
}

// ---- assembler ----

void test_assembler_completes_on_the_fifth_byte() {
  CatFrameAssembler a;
  uint8_t out[5];
  const uint8_t f[5] = {1, 2, 3, 4, 5};
  for (int i = 0; i < 4; i++) {
    TEST_ASSERT_FALSE(a.push(f[i], i, out));
    TEST_ASSERT_FALSE(a.idle());
  }
  TEST_ASSERT_TRUE(a.push(f[4], 4, out));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f, out, 5);
  TEST_ASSERT_TRUE(a.idle());
}

void test_assembler_drops_a_stalled_partial_frame() {
  CatFrameAssembler a;
  uint8_t out[5];
  a.push(9, 0, out);
  a.push(9, 10, out);
  TEST_ASSERT_FALSE(a.poll(209));
  TEST_ASSERT_TRUE(a.poll(210)); // 200ms after the last byte
  TEST_ASSERT_TRUE(a.idle());
  TEST_ASSERT_FALSE(a.poll(211)); // reported once
}

void test_assembler_resyncs_if_poll_did_not_run() {
  CatFrameAssembler a;
  uint8_t out[5];
  a.push(0xAA, 0, out);
  const uint8_t f[5] = {1, 2, 3, 4, 5};
  bool done = false;
  for (int i = 0; i < 5; i++) done = a.push(f[i], 1000 + i, out); // stale byte must not leak in
  TEST_ASSERT_TRUE(done);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f, out, 5);
}

void test_assembler_keeps_a_slow_but_legal_frame() {
  CatFrameAssembler a;
  uint8_t out[5];
  bool done = false;
  for (int i = 0; i < 5; i++) done = a.push(i, i * 199, out);
  TEST_ASSERT_TRUE(done);
}

void test_assembler_wraparound() {
  CatFrameAssembler a;
  uint8_t out[5];
  uint32_t t = 0xFFFFFFFCu;
  bool done = false;
  for (int i = 0; i < 5; i++) done = a.push(i, t + i, out);
  TEST_ASSERT_TRUE(done);
}

// ---- replay queue ----

static void frame(uint8_t *f, uint8_t tag) { f[0] = tag; f[1] = f[2] = f[3] = 0; f[4] = 0x07; }

void test_queue_is_first_in_first_out() {
  CatReplayQueue q;
  uint8_t f[5], o[5];
  for (uint8_t i = 1; i <= 3; i++) { frame(f, i); TEST_ASSERT_FALSE(q.push(f)); }
  TEST_ASSERT_EQUAL(3, q.count());
  for (uint8_t i = 1; i <= 3; i++) { TEST_ASSERT_TRUE(q.pop(o)); TEST_ASSERT_EQUAL(i, o[0]); }
  TEST_ASSERT_FALSE(q.pop(o));
}

void test_queue_overflow_drops_the_oldest_and_keeps_the_newest() {
  CatReplayQueue q;
  uint8_t f[5], o[5];
  for (uint8_t i = 1; i <= CAT_REPLAY_MAX; i++) { frame(f, i); TEST_ASSERT_FALSE(q.push(f)); }
  frame(f, 100);
  TEST_ASSERT_TRUE(q.push(f)); // full: oldest (1) goes
  TEST_ASSERT_EQUAL(CAT_REPLAY_MAX, q.count());
  TEST_ASSERT_EQUAL(1, q.dropped());
  TEST_ASSERT_TRUE(q.pop(o));
  TEST_ASSERT_EQUAL(2, o[0]);
  uint8_t last = 0;
  while (q.pop(o)) last = o[0];
  TEST_ASSERT_EQUAL(100, last); // the newest survived
}

void test_queue_wraps_around_correctly() {
  CatReplayQueue q;
  uint8_t f[5], o[5];
  for (int round = 0; round < 5; round++) {
    for (uint8_t i = 0; i < 6; i++) { frame(f, round * 10 + i); q.push(f); }
    for (uint8_t i = 0; i < 6; i++) { TEST_ASSERT_TRUE(q.pop(o)); TEST_ASSERT_EQUAL(round * 10 + i, o[0]); }
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_freq_mode_query_gets_the_original_mode);
  RUN_TEST(test_status_queries_get_the_snapshot);
  RUN_TEST(test_ptt_commands_are_swallowed);
  RUN_TEST(test_other_commands_are_queued_for_after_the_tune);
  RUN_TEST(test_queries_without_a_snapshot_field_are_queued_not_invented);
  RUN_TEST(test_assembler_completes_on_the_fifth_byte);
  RUN_TEST(test_assembler_drops_a_stalled_partial_frame);
  RUN_TEST(test_assembler_resyncs_if_poll_did_not_run);
  RUN_TEST(test_assembler_keeps_a_slow_but_legal_frame);
  RUN_TEST(test_assembler_wraparound);
  RUN_TEST(test_queue_is_first_in_first_out);
  RUN_TEST(test_queue_overflow_drops_the_oldest_and_keeps_the_newest);
  RUN_TEST(test_queue_wraps_around_correctly);
  return UNITY_END();
}
