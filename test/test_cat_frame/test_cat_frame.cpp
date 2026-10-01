#include <unity.h>

#include "cat_frame.h"

static CatFramer f;

void setUp() { f = CatFramer(); }

// Feed a whole 5-byte command, 1ms apart starting at t; returns the last event.
static CatEvent sendCmd(const uint8_t (&c)[5], uint32_t t) {
  CatEvent ev;
  for (int i = 0; i < 5; i++) ev = f.pcByte(c[i], t + i);
  return ev;
}

static const uint8_t GET_FREQ[5] = {0, 0, 0, 0, 0x03};
static const uint8_t GET_TX[5] = {0, 0, 0, 0, 0xF7};
static const uint8_t PTT_ON[5] = {0, 0, 0, 0, 0x08};

void test_reply_lengths() {
  TEST_ASSERT_EQUAL(1, catReplyLength(0xE7));
  TEST_ASSERT_EQUAL(1, catReplyLength(0xF7));
  TEST_ASSERT_EQUAL(5, catReplyLength(0x03));
  TEST_ASSERT_EQUAL(5, catReplyLength(0x13));
  TEST_ASSERT_EQUAL(5, catReplyLength(0x23));
  // everything else in the manual's opcode chart: no reply
  const uint8_t none[] = {0x00, 0x80, 0x08, 0x88, 0x4E, 0x8E, 0x01, 0x11, 0x21, 0x07, 0x17, 0x27,
                          0x0A, 0x1A, 0x2A, 0x0B, 0x1B, 0x2B, 0x0C, 0x1C, 0x2C, 0x09, 0xF9};
  for (uint8_t op : none) TEST_ASSERT_EQUAL(0, catReplyLength(op));
}

void test_command_completes_on_fifth_byte() {
  for (int i = 0; i < 4; i++) {
    TEST_ASSERT_EQUAL(CatEvent::None, f.pcByte(GET_FREQ[i], i).kind);
  }
  CatEvent ev = f.pcByte(GET_FREQ[4], 4);
  TEST_ASSERT_EQUAL(CatEvent::Command, ev.kind);
  TEST_ASSERT_EQUAL(0x03, ev.opcode);
  TEST_ASSERT_EQUAL(5, ev.len);
  TEST_ASSERT_TRUE(f.replyPending());
}

void test_five_byte_reply() {
  sendCmd(GET_FREQ, 0);
  const uint8_t r[5] = {0x14, 0x42, 0x50, 0x00, 0x01};
  for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL(CatEvent::None, f.radioByte(r[i], 10 + i).kind);
  CatEvent ev = f.radioByte(r[4], 14);
  TEST_ASSERT_EQUAL(CatEvent::Reply, ev.kind);
  TEST_ASSERT_EQUAL(0x03, ev.opcode);
  TEST_ASSERT_EQUAL(5, ev.len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(r, ev.data, 5);
  TEST_ASSERT_FALSE(f.replyPending());
}

void test_one_byte_reply() {
  sendCmd(GET_TX, 0);
  CatEvent ev = f.radioByte(0x80, 5);
  TEST_ASSERT_EQUAL(CatEvent::Reply, ev.kind);
  TEST_ASSERT_EQUAL(0xF7, ev.opcode);
  TEST_ASSERT_EQUAL(1, ev.len);
  TEST_ASSERT_EQUAL(0x80, ev.data[0]);
}

void test_set_command_expects_no_reply() {
  CatEvent ev = sendCmd(PTT_ON, 0);
  TEST_ASSERT_EQUAL(CatEvent::Command, ev.kind);
  TEST_ASSERT_FALSE(f.replyPending());
}

void test_stray_radio_byte() {
  CatEvent ev = f.radioByte(0x55, 0);
  TEST_ASSERT_EQUAL(CatEvent::StrayReply, ev.kind);
  TEST_ASSERT_EQUAL(0x55, ev.data[0]);
}

void test_back_to_back_commands() {
  CatEvent a = sendCmd(PTT_ON, 0);
  CatEvent b = sendCmd(GET_TX, 5);
  TEST_ASSERT_EQUAL(CatEvent::Command, a.kind);
  TEST_ASSERT_EQUAL(CatEvent::Command, b.kind);
  TEST_ASSERT_EQUAL(0xF7, b.opcode);
}

void test_torn_command_abandoned_by_poll() {
  f.pcByte(0x01, 0);
  f.pcByte(0x02, 10);
  TEST_ASSERT_EQUAL(CatEvent::None, f.poll(209).kind);
  CatEvent ev = f.poll(210); // 200ms after the last byte
  TEST_ASSERT_EQUAL(CatEvent::CommandAbandoned, ev.kind);
  TEST_ASSERT_EQUAL(2, ev.len);
  TEST_ASSERT_EQUAL(0x01, ev.data[0]);
  TEST_ASSERT_EQUAL(CatEvent::None, f.poll(211).kind); // reported once
}

void test_torn_command_resyncs_on_next_frame() {
  f.pcByte(0xAA, 0);
  f.pcByte(0xBB, 1);
  // PC gives up and sends a fresh command much later, without poll() having run
  CatEvent ev = sendCmd(GET_TX, 1000);
  TEST_ASSERT_EQUAL(CatEvent::Command, ev.kind);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(GET_TX, ev.data, 5); // stale bytes did not leak in
}

void test_slow_but_legal_command_is_kept() {
  for (int i = 0; i < 4; i++) f.pcByte(0, i * 199); // 199ms gaps are allowed
  CatEvent ev = f.pcByte(0x08, 4 * 199);
  TEST_ASSERT_EQUAL(CatEvent::Command, ev.kind);
}

void test_reply_timeout() {
  sendCmd(GET_FREQ, 0);
  f.radioByte(0x14, 100);
  f.radioByte(0x42, 101);
  TEST_ASSERT_EQUAL(CatEvent::None, f.poll(2003).kind);
  CatEvent ev = f.poll(2004); // 2000ms after the command completed (t=4)
  TEST_ASSERT_EQUAL(CatEvent::ReplyAbandoned, ev.kind);
  TEST_ASSERT_EQUAL(0x03, ev.opcode);
  TEST_ASSERT_EQUAL(2, ev.len);
  TEST_ASSERT_FALSE(f.replyPending());
  // a late byte is now stray, not part of a reply
  TEST_ASSERT_EQUAL(CatEvent::StrayReply, f.radioByte(0x00, 2100).kind);
}

void test_new_request_supersedes_unfinished_reply() {
  sendCmd(GET_FREQ, 0);
  f.radioByte(0x14, 10); // partial reply
  sendCmd(GET_TX, 20);
  CatEvent ev = f.radioByte(0x80, 30);
  TEST_ASSERT_EQUAL(CatEvent::Reply, ev.kind);
  TEST_ASSERT_EQUAL(0xF7, ev.opcode);
  TEST_ASSERT_EQUAL(1, ev.len);
}

void test_millis_wraparound() {
  uint32_t t = 0xFFFFFFFCu;
  CatEvent ev = sendCmd(GET_TX, t); // bytes straddle the 32-bit wrap
  TEST_ASSERT_EQUAL(CatEvent::Command, ev.kind);
  TEST_ASSERT_EQUAL(CatEvent::None, f.poll(t + 1000).kind);
  TEST_ASSERT_EQUAL(CatEvent::None, f.poll(t + 1999).kind);
  TEST_ASSERT_EQUAL(CatEvent::ReplyAbandoned, f.poll(t + 2004).kind);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_reply_lengths);
  RUN_TEST(test_command_completes_on_fifth_byte);
  RUN_TEST(test_five_byte_reply);
  RUN_TEST(test_one_byte_reply);
  RUN_TEST(test_set_command_expects_no_reply);
  RUN_TEST(test_stray_radio_byte);
  RUN_TEST(test_back_to_back_commands);
  RUN_TEST(test_torn_command_abandoned_by_poll);
  RUN_TEST(test_torn_command_resyncs_on_next_frame);
  RUN_TEST(test_slow_but_legal_command_is_kept);
  RUN_TEST(test_reply_timeout);
  RUN_TEST(test_new_request_supersedes_unfinished_reply);
  RUN_TEST(test_millis_wraparound);
  return UNITY_END();
}
