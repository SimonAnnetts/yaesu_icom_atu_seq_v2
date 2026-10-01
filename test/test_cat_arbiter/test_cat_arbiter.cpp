#include <unity.h>

#include "cat_arbiter.h"

static CatArbiter a;
static const uint8_t GET_FREQ[5] = {0, 0, 0, 0, 0x03};
static const uint8_t SET_MODE[5] = {0x01, 0, 0, 0, 0x07};

void setUp() { a = CatArbiter(); }

// Poll once per ms from `from` to `to`, with the PC bus state given.
static void run(uint32_t from, uint32_t to, bool pcIdle = true) {
  for (uint32_t t = from; t <= to; t++) a.poll(t, pcIdle);
}

void test_idle_bus_lets_pc_through() {
  TEST_ASSERT_TRUE(a.pcMayTransmit());
  TEST_ASSERT_FALSE(a.busy());
}

void test_query_on_a_quiet_bus() {
  a.noteBusActivity(0);
  TEST_ASSERT_TRUE(a.submit(GET_FREQ, 10));
  TEST_ASSERT_TRUE(a.busy());
  a.poll(10, true);
  TEST_ASSERT_FALSE(a.pcMayTransmit());       // PC held as soon as the bus is free
  TEST_ASSERT_NULL(a.pendingSend());          // ...but the 50ms quiet gap hasn't passed
  run(11, 49);
  TEST_ASSERT_NULL(a.pendingSend());
  run(50, 50);
  TEST_ASSERT_NOT_NULL(a.pendingSend());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(GET_FREQ, a.pendingSend(), 5);
  a.sent(51);
  TEST_ASSERT_NULL(a.pendingSend());
  TEST_ASSERT_TRUE(a.ownsRadioReplies());
  const uint8_t r[5] = {0x14, 0x42, 0x50, 0x00, 0x01};
  for (int i = 0; i < 5; i++) a.radioByte(r[i], 60 + i);
  TEST_ASSERT_FALSE(a.ownsRadioReplies());
  CatArbiter::Result res;
  uint8_t reply[5];
  uint8_t len = 0;
  TEST_ASSERT_TRUE(a.takeResult(res, reply, len));
  TEST_ASSERT_EQUAL(CatArbiter::Result::Ok, res);
  TEST_ASSERT_EQUAL(5, len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(r, reply, 5);
  TEST_ASSERT_FALSE(a.takeResult(res, reply, len)); // result is delivered once
  // PC stays held through the post-command quiet gap
  a.poll(100, true);
  TEST_ASSERT_FALSE(a.pcMayTransmit());
  a.poll(114, true);
  TEST_ASSERT_TRUE(a.pcMayTransmit()); // 60+4=64 finished, +50 = 114
  TEST_ASSERT_FALSE(a.busy());
}

void test_command_without_reply_completes_at_once() {
  TEST_ASSERT_TRUE(a.submit(SET_MODE, 0));
  run(0, 60);
  TEST_ASSERT_NOT_NULL(a.pendingSend());
  a.sent(61);
  CatArbiter::Result res;
  uint8_t reply[5];
  uint8_t len = 9;
  TEST_ASSERT_TRUE(a.takeResult(res, reply, len));
  TEST_ASSERT_EQUAL(CatArbiter::Result::Ok, res);
  TEST_ASSERT_EQUAL(0, len);
  TEST_ASSERT_FALSE(a.pcMayTransmit()); // still in the post-send quiet gap
  run(62, 111);
  TEST_ASSERT_TRUE(a.pcMayTransmit());
}

void test_waits_for_pc_exchange_but_does_not_block_it() {
  TEST_ASSERT_TRUE(a.submit(GET_FREQ, 0));
  run(0, 500, false); // PC mid-exchange the whole time
  TEST_ASSERT_TRUE(a.pcMayTransmit());   // PC keeps flowing so its exchange can finish
  TEST_ASSERT_NULL(a.pendingSend());
  a.noteBusActivity(500);                // last byte of the PC's reply
  a.poll(500, true);                     // exchange done: gate closes
  TEST_ASSERT_FALSE(a.pcMayTransmit());  // next PC command is held back
  run(501, 549);
  TEST_ASSERT_NULL(a.pendingSend());
  run(550, 550);
  TEST_ASSERT_NOT_NULL(a.pendingSend()); // 50ms after the PC's last byte
}

void test_pc_traffic_never_idle_times_out() {
  TEST_ASSERT_TRUE(a.submit(GET_FREQ, 0));
  run(0, ARB_BUS_TIMEOUT_MS - 1, false);
  TEST_ASSERT_TRUE(a.busy());
  a.poll(ARB_BUS_TIMEOUT_MS, false);
  CatArbiter::Result res;
  uint8_t reply[5];
  uint8_t len;
  TEST_ASSERT_TRUE(a.takeResult(res, reply, len));
  TEST_ASSERT_EQUAL(CatArbiter::Result::BusTimeout, res);
  TEST_ASSERT_FALSE(a.busy());
  TEST_ASSERT_TRUE(a.pcMayTransmit());
}

void test_reply_timeout_reports_partial() {
  TEST_ASSERT_TRUE(a.submit(GET_FREQ, 0));
  run(0, 60);
  a.sent(61);
  a.radioByte(0x14, 70);
  a.radioByte(0x42, 71);
  a.poll(560, true);
  TEST_ASSERT_TRUE(a.ownsRadioReplies());
  a.poll(561, true); // 500ms after sending
  CatArbiter::Result res;
  uint8_t reply[5];
  uint8_t len = 0;
  TEST_ASSERT_TRUE(a.takeResult(res, reply, len));
  TEST_ASSERT_EQUAL(CatArbiter::Result::NoReply, res);
  TEST_ASSERT_EQUAL(2, len);
  TEST_ASSERT_FALSE(a.ownsRadioReplies());
}

void test_second_submit_rejected_while_busy() {
  TEST_ASSERT_TRUE(a.submit(GET_FREQ, 0));
  TEST_ASSERT_FALSE(a.submit(SET_MODE, 1));
}

void test_new_transaction_after_hold() {
  TEST_ASSERT_TRUE(a.submit(SET_MODE, 0));
  run(0, 60);
  a.sent(61);
  run(62, 111);
  TEST_ASSERT_TRUE(a.submit(GET_FREQ, 112));
}

void test_millis_wraparound() {
  uint32_t t0 = 0xFFFFFFF0u;
  a.noteBusActivity(t0);
  TEST_ASSERT_TRUE(a.submit(SET_MODE, t0));
  for (uint32_t i = 0; i < 49; i++) a.poll(t0 + i, true);
  TEST_ASSERT_NULL(a.pendingSend());
  a.poll(t0 + 50, true);
  TEST_ASSERT_NOT_NULL(a.pendingSend());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_idle_bus_lets_pc_through);
  RUN_TEST(test_query_on_a_quiet_bus);
  RUN_TEST(test_command_without_reply_completes_at_once);
  RUN_TEST(test_waits_for_pc_exchange_but_does_not_block_it);
  RUN_TEST(test_pc_traffic_never_idle_times_out);
  RUN_TEST(test_reply_timeout_reports_partial);
  RUN_TEST(test_second_submit_rejected_while_busy);
  RUN_TEST(test_new_transaction_after_hold);
  RUN_TEST(test_millis_wraparound);
  return UNITY_END();
}
