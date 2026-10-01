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


using Claim = CatArbiter::ClaimState;

void test_claim_on_a_quiet_bus() {
  a.noteBusActivity(0);
  TEST_ASSERT_TRUE(a.claim(10));
  TEST_ASSERT_EQUAL(Claim::Pending, a.claimState());
  a.poll(10, true);
  TEST_ASSERT_FALSE(a.pcMayTransmit()); // PC held as soon as it is between exchanges
  run(11, 49);
  TEST_ASSERT_EQUAL(Claim::Pending, a.claimState()); // quiet gap not yet passed
  run(50, 50);
  TEST_ASSERT_EQUAL(Claim::Held, a.claimState());
  TEST_ASSERT_FALSE(a.pcMayTransmit());
}

void test_claim_waits_for_the_pc_exchange_but_does_not_block_it() {
  TEST_ASSERT_TRUE(a.claim(0));
  run(0, 300, false); // PC mid-exchange
  TEST_ASSERT_TRUE(a.pcMayTransmit());
  TEST_ASSERT_EQUAL(Claim::Pending, a.claimState());
  a.noteBusActivity(300);
  a.poll(300, true);
  TEST_ASSERT_FALSE(a.pcMayTransmit());
  run(301, 350);
  TEST_ASSERT_EQUAL(Claim::Held, a.claimState());
}

void test_claim_times_out_and_gives_the_bus_back() {
  TEST_ASSERT_TRUE(a.claim(0));
  run(0, ARB_CLAIM_TIMEOUT_MS - 1, false);
  TEST_ASSERT_EQUAL(Claim::Pending, a.claimState());
  a.poll(ARB_CLAIM_TIMEOUT_MS, false);
  TEST_ASSERT_EQUAL(Claim::Failed, a.claimState());
  TEST_ASSERT_TRUE(a.pcMayTransmit());
  a.releaseClaim(ARB_CLAIM_TIMEOUT_MS + 1); // clears the failure
  TEST_ASSERT_EQUAL(Claim::None, a.claimState());
  TEST_ASSERT_TRUE(a.claim(ARB_CLAIM_TIMEOUT_MS + 2)); // can try again
}

void test_claim_rejected_twice() {
  TEST_ASSERT_TRUE(a.claim(0));
  TEST_ASSERT_FALSE(a.claim(1));
  run(0, 100);
  TEST_ASSERT_EQUAL(Claim::Held, a.claimState());
  TEST_ASSERT_FALSE(a.claim(101));
}

void test_submit_rejected_while_claim_pending() {
  TEST_ASSERT_TRUE(a.claim(0));
  TEST_ASSERT_FALSE(a.submit(SET_MODE, 1));
}

void test_commands_while_claimed_go_straight_out() {
  const uint8_t PTT_ON[5] = {0, 0, 0, 0, 0x08};
  TEST_ASSERT_TRUE(a.claim(0));
  run(0, 60);
  TEST_ASSERT_EQUAL(Claim::Held, a.claimState());
  // Bus has been quiet far longer than 50ms: no wait at all.
  TEST_ASSERT_TRUE(a.submit(PTT_ON, 5000));
  a.poll(5000, true);
  TEST_ASSERT_NOT_NULL(a.pendingSend());
  a.sent(5001);
  CatArbiter::Result res;
  uint8_t reply[5];
  uint8_t len;
  TEST_ASSERT_TRUE(a.takeResult(res, reply, len));
  TEST_ASSERT_EQUAL(CatArbiter::Result::Ok, res);
  TEST_ASSERT_FALSE(a.pcMayTransmit());              // still ours: no post-command hold needed
  TEST_ASSERT_EQUAL(Claim::Held, a.claimState());
}

void test_back_to_back_claimed_commands_are_spaced() {
  TEST_ASSERT_TRUE(a.claim(0));
  run(0, 60);
  TEST_ASSERT_TRUE(a.submit(SET_MODE, 1000));
  a.poll(1000, true);
  a.sent(1001);
  CatArbiter::Result res;
  uint8_t reply[5];
  uint8_t len;
  TEST_ASSERT_TRUE(a.takeResult(res, reply, len));
  TEST_ASSERT_TRUE(a.submit(SET_MODE, 1002));
  run(1002, 1050);
  TEST_ASSERT_NULL(a.pendingSend()); // 49ms after the last send
  run(1051, 1051);
  TEST_ASSERT_NOT_NULL(a.pendingSend());
}

void test_claimed_query_gets_its_reply() {
  TEST_ASSERT_TRUE(a.claim(0));
  run(0, 60);
  TEST_ASSERT_TRUE(a.submit(GET_FREQ, 100));
  run(100, 160);
  TEST_ASSERT_NOT_NULL(a.pendingSend());
  a.sent(161);
  TEST_ASSERT_TRUE(a.ownsRadioReplies());
  const uint8_t r[5] = {0x14, 0x42, 0x50, 0x00, 0x04};
  for (int i = 0; i < 5; i++) a.radioByte(r[i], 170 + i);
  CatArbiter::Result res;
  uint8_t reply[5];
  uint8_t len = 0;
  TEST_ASSERT_TRUE(a.takeResult(res, reply, len));
  TEST_ASSERT_EQUAL(CatArbiter::Result::Ok, res);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(r, reply, 5);
  TEST_ASSERT_EQUAL(Claim::Held, a.claimState());
}

void test_release_hands_back_after_a_quiet_gap() {
  TEST_ASSERT_TRUE(a.claim(0));
  run(0, 60);
  a.releaseClaim(100);
  TEST_ASSERT_EQUAL(Claim::None, a.claimState());
  a.poll(100, true);
  TEST_ASSERT_FALSE(a.pcMayTransmit());
  a.poll(149, true);
  TEST_ASSERT_FALSE(a.pcMayTransmit());
  a.poll(150, true);
  TEST_ASSERT_TRUE(a.pcMayTransmit());
}

void test_forgotten_claim_releases_itself() {
  TEST_ASSERT_TRUE(a.claim(0));
  run(0, 60); // acquired at t=50
  a.poll(50 + ARB_CLAIM_MAX_MS - 1, true);
  TEST_ASSERT_EQUAL(Claim::Held, a.claimState());
  a.poll(50 + ARB_CLAIM_MAX_MS, true);
  TEST_ASSERT_EQUAL(Claim::None, a.claimState());
  a.poll(50 + ARB_CLAIM_MAX_MS + ARB_QUIET_MS, true);
  TEST_ASSERT_TRUE(a.pcMayTransmit());
}

void test_claim_waits_for_a_running_transaction() {
  TEST_ASSERT_TRUE(a.submit(GET_FREQ, 0));
  run(0, 60);
  a.sent(61);
  TEST_ASSERT_TRUE(a.claim(62));
  run(62, 300);
  TEST_ASSERT_EQUAL(Claim::Pending, a.claimState()); // reply still outstanding
  const uint8_t r[5] = {0, 0, 0, 0, 1};
  for (int i = 0; i < 5; i++) a.radioByte(r[i], 301 + i);
  run(305, 500);
  TEST_ASSERT_EQUAL(Claim::Held, a.claimState());
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
  RUN_TEST(test_claim_on_a_quiet_bus);
  RUN_TEST(test_claim_waits_for_the_pc_exchange_but_does_not_block_it);
  RUN_TEST(test_claim_times_out_and_gives_the_bus_back);
  RUN_TEST(test_claim_rejected_twice);
  RUN_TEST(test_submit_rejected_while_claim_pending);
  RUN_TEST(test_commands_while_claimed_go_straight_out);
  RUN_TEST(test_back_to_back_claimed_commands_are_spaced);
  RUN_TEST(test_claimed_query_gets_its_reply);
  RUN_TEST(test_release_hands_back_after_a_quiet_gap);
  RUN_TEST(test_forgotten_claim_releases_itself);
  RUN_TEST(test_claim_waits_for_a_running_transaction);
  return UNITY_END();
}
