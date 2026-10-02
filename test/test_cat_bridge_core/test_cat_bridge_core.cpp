#include <string.h>
#include <unity.h>

#include <vector>

#include "cat_bridge_core.h"

using Mode = CatBridgeCore::Mode;

struct Io : CatBridgeIo {
  std::vector<uint8_t> pcIn, radioIn;   // bytes waiting to be read
  std::vector<uint8_t> toRadio, toPc;   // bytes written
  size_t pcPos = 0, radioPos = 0;
  int torn = 0, stray = 0, timeouts = 0;
  int pcRead() override { return pcPos < pcIn.size() ? pcIn[pcPos++] : -1; }
  int radioRead() override { return radioPos < radioIn.size() ? radioIn[radioPos++] : -1; }
  void pcWrite(uint8_t b) override { toPc.push_back(b); }
  void radioWrite(uint8_t b) override { toRadio.push_back(b); }
  void frame(Frame k, const uint8_t *, uint8_t) override {
    if (k == Frame::TornFromPc) torn++;
    if (k == Frame::StrayFromRadio) stray++;
    if (k == Frame::ReplyTimeout) timeouts++;
  }
  size_t pcUnread() const { return pcIn.size() - pcPos; }
};

static Io io;
static CatBridgeCore *core;

void setUp() {
  io = Io();
  delete core;
  core = new CatBridgeCore(io);
}

static void run(uint32_t from, uint32_t to) {
  for (uint32_t t = from; t <= to; t++) core->poll(t);
}

void test_passthrough_is_byte_transparent_in_both_directions() {
  // arbitrary bytes, not even valid frames: nothing may be altered, dropped or reordered
  for (int i = 0; i < 37; i++) io.pcIn.push_back((uint8_t)(i * 7 + 3));
  for (int i = 0; i < 23; i++) io.radioIn.push_back((uint8_t)(255 - i * 5));
  run(0, 10);
  TEST_ASSERT_EQUAL(37, io.toRadio.size());
  TEST_ASSERT_EQUAL(23, io.toPc.size());
  for (int i = 0; i < 37; i++) TEST_ASSERT_EQUAL_UINT8((uint8_t)(i * 7 + 3), io.toRadio[i]);
  for (int i = 0; i < 23; i++) TEST_ASSERT_EQUAL_UINT8((uint8_t)(255 - i * 5), io.toPc[i]);
  TEST_ASSERT_EQUAL((int)Mode::Normal, (int)core->mode());
}

void test_poll_reports_whether_bytes_moved() {
  TEST_ASSERT_FALSE(core->poll(0));
  io.pcIn.push_back(0x55);
  TEST_ASSERT_TRUE(core->poll(1));
  TEST_ASSERT_FALSE(core->poll(2));
  io.radioIn.push_back(0x66);
  TEST_ASSERT_TRUE(core->poll(3));
}

void test_a_torn_pc_command_is_reported_but_still_forwarded() {
  io.pcIn = {1, 2, 3};
  run(0, 300);
  TEST_ASSERT_EQUAL(3, io.toRadio.size()); // transparent: forwarded as it came
  TEST_ASSERT_EQUAL(1, io.torn);
}

void test_the_arduino_command_goes_out_and_its_reply_stays_off_the_pc() {
  const uint8_t q[5] = {0, 0, 0, 0, 0x03};
  TEST_ASSERT_TRUE(core->submit(q, 0));
  run(0, 80);
  TEST_ASSERT_EQUAL(5, io.toRadio.size());
  io.radioIn = {0x14, 0x42, 0x50, 0x00, 0x01};
  run(81, 90);
  TEST_ASSERT_EQUAL(0, io.toPc.size()); // the PC never sees the Arduino's exchange
  CatArbiter::Result r;
  uint8_t reply[5];
  uint8_t len = 0;
  TEST_ASSERT_TRUE(core->takeResult(r, reply, len));
  TEST_ASSERT_EQUAL(5, len);
  TEST_ASSERT_EQUAL(0x14, reply[0]);
}

void test_claim_holds_pc_bytes_unread_until_there_is_a_snapshot() {
  TEST_ASSERT_TRUE(core->claim(0));
  run(0, 100);
  TEST_ASSERT_EQUAL((int)CatArbiter::ClaimState::Held, (int)core->claimState());
  TEST_ASSERT_EQUAL((int)Mode::Hold, (int)core->mode());
  io.pcIn = {0, 0, 0, 0, 0x03}; // a PC query arrives meanwhile
  run(101, 150);
  TEST_ASSERT_EQUAL(5, io.pcUnread()); // left in the port's buffer, not consumed
  TEST_ASSERT_EQUAL(0, io.toRadio.size());
  CatSnapshot s;
  s.haveFreqMode = true;
  const uint8_t fm[5] = {0x01, 0x42, 0x50, 0x00, 0x01};
  memcpy(s.freqMode, fm, 5);
  core->setSnapshot(s);
  run(151, 160);
  TEST_ASSERT_EQUAL((int)Mode::Intercept, (int)core->mode());
  TEST_ASSERT_EQUAL(0, io.pcUnread());
  TEST_ASSERT_EQUAL(5, io.toPc.size()); // answered at once, from the snapshot
  TEST_ASSERT_EQUAL_UINT8_ARRAY(fm, io.toPc.data(), 5);
  TEST_ASSERT_EQUAL(0, io.toRadio.size()); // never reached the radio
}

void test_a_claim_that_ends_with_no_snapshot_just_resumes_normal_traffic() {
  TEST_ASSERT_TRUE(core->claim(0));
  run(0, 100);
  io.pcIn = {0, 0, 0, 0, 0xF7};
  run(101, 120);
  TEST_ASSERT_EQUAL(5, io.pcUnread());
  core->releaseClaim(121); // a refused tune: the snapshot never came
  run(121, 260);
  TEST_ASSERT_EQUAL((int)Mode::Normal, (int)core->mode());
  TEST_ASSERT_EQUAL(0, io.pcUnread());
  TEST_ASSERT_EQUAL(5, io.toRadio.size()); // the held query went out, intact, afterwards
  TEST_ASSERT_EQUAL(0xF7, io.toRadio[4]);
}

void test_stray_radio_bytes_are_not_passed_to_the_pc_while_the_bus_is_claimed() {
  TEST_ASSERT_TRUE(core->claim(0));
  run(0, 100);
  io.radioIn = {0xAA};
  run(101, 110);
  TEST_ASSERT_EQUAL(0, io.toPc.size());
  TEST_ASSERT_EQUAL(1, io.stray);
}

void test_the_claim_waits_for_a_pc_exchange_in_flight() {
  io.pcIn = {0, 0, 0, 0, 0x03}; // PC query goes out; the radio has not answered yet
  run(0, 5);
  TEST_ASSERT_EQUAL(5, io.toRadio.size());
  TEST_ASSERT_TRUE(core->claim(6));
  run(6, 500);
  TEST_ASSERT_EQUAL((int)CatArbiter::ClaimState::Pending, (int)core->claimState());
  io.radioIn = {0x14, 0x42, 0x50, 0x00, 0x01}; // the reply arrives: now the bus is free
  run(501, 700);
  TEST_ASSERT_EQUAL(5, io.toPc.size()); // and the reply reached the PC
  TEST_ASSERT_EQUAL((int)CatArbiter::ClaimState::Held, (int)core->claimState());
}

void test_millis_wraparound() {
  uint32_t t0 = 0xFFFFFF00u;
  TEST_ASSERT_TRUE(core->claim(t0));
  for (uint32_t i = 0; i < 400; i++) core->poll(t0 + i);
  TEST_ASSERT_EQUAL((int)CatArbiter::ClaimState::Held, (int)core->claimState());
  CatSnapshot s;
  s.haveTx = true;
  s.tx = 0x80;
  core->setSnapshot(s);
  io.pcIn = {0, 0, 0, 0, 0xF7};
  for (uint32_t i = 400; i < 420; i++) core->poll(t0 + i);
  TEST_ASSERT_EQUAL(1, io.toPc.size());
  TEST_ASSERT_EQUAL(0x80, io.toPc[0]);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_passthrough_is_byte_transparent_in_both_directions);
  RUN_TEST(test_poll_reports_whether_bytes_moved);
  RUN_TEST(test_a_torn_pc_command_is_reported_but_still_forwarded);
  RUN_TEST(test_the_arduino_command_goes_out_and_its_reply_stays_off_the_pc);
  RUN_TEST(test_claim_holds_pc_bytes_unread_until_there_is_a_snapshot);
  RUN_TEST(test_a_claim_that_ends_with_no_snapshot_just_resumes_normal_traffic);
  RUN_TEST(test_stray_radio_bytes_are_not_passed_to_the_pc_while_the_bus_is_claimed);
  RUN_TEST(test_the_claim_waits_for_a_pc_exchange_in_flight);
  RUN_TEST(test_millis_wraparound);
  return UNITY_END();
}
