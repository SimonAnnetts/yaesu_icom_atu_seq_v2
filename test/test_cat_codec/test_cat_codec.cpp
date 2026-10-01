#include <unity.h>

#include "cat_codec.h"
#include "cat_frame.h"

void setUp() {}

void test_decode_freq() {
  const uint8_t f[4] = {0x43, 0x21, 0x00, 0x00};
  uint32_t hz = 0;
  TEST_ASSERT_TRUE(catDecodeFreq(f, hz));
  TEST_ASSERT_EQUAL_UINT32(432100000, hz);
  const uint8_t hf[4] = {0x01, 0x42, 0x50, 0x00}; // 14.25MHz
  TEST_ASSERT_TRUE(catDecodeFreq(hf, hz));
  TEST_ASSERT_EQUAL_UINT32(14250000, hz);
}

void test_decode_rejects_non_bcd() {
  const uint8_t bad[4] = {0x0A, 0x00, 0x00, 0x00};
  uint32_t hz = 123;
  TEST_ASSERT_FALSE(catDecodeFreq(bad, hz));
  const uint8_t bad2[4] = {0x00, 0x00, 0x00, 0xF0};
  TEST_ASSERT_FALSE(catDecodeFreq(bad2, hz));
}

void test_encode_freq() {
  uint8_t b[4];
  catEncodeFreq(439700000, b); // the manual's example: 439.70MHz
  const uint8_t want[4] = {0x43, 0x97, 0x00, 0x00};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want, b, 4);
  catEncodeFreq(1840000, b);
  const uint8_t want2[4] = {0x00, 0x18, 0x40, 0x00};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want2, b, 4);
}

void test_freq_roundtrip_and_10hz_rounding() {
  const uint32_t freqs[] = {1800000, 7074000, 14250000, 50313000, 144174000, 432100000, 439700010};
  for (uint32_t f : freqs) {
    uint8_t b[4];
    uint32_t hz = 0;
    catEncodeFreq(f, b);
    TEST_ASSERT_TRUE(catDecodeFreq(b, hz));
    TEST_ASSERT_EQUAL_UINT32(f, hz);
  }
  uint8_t b[4];
  uint32_t hz = 0;
  catEncodeFreq(14250007, b); // sub-10Hz digit is dropped
  catDecodeFreq(b, hz);
  TEST_ASSERT_EQUAL_UINT32(14250000, hz);
}

void test_modes_roundtrip_including_narrow() {
  const uint8_t modes[] = {MODE_LSB, MODE_USB, MODE_CW, MODE_CWR, MODE_AM, MODE_FM,
                           MODE_CWN, MODE_CWRN, MODE_AMN, MODE_FMN};
  for (uint8_t m : modes) {
    TEST_ASSERT_TRUE(catModeValid(m));
    uint8_t cmd[5];
    catCmdSetMode(cmd, m); // restore of a mode byte read from a 0x03 reply is exact
    TEST_ASSERT_EQUAL(m, cmd[0]);
    TEST_ASSERT_EQUAL(0x07, cmd[4]);
  }
  TEST_ASSERT_FALSE(catModeValid(0x05));
  TEST_ASSERT_FALSE(catModeValid(0x81));
  TEST_ASSERT_EQUAL_STRING("AMN", catModeName(MODE_AMN));
  TEST_ASSERT_EQUAL_STRING("?", catModeName(0x55));
}

void test_tx_status_polarity() {
  TEST_ASSERT_FALSE(catTxStatusTransmitting(0x80)); // bit 7 set = not transmitting
  TEST_ASSERT_FALSE(catTxStatusTransmitting(0x9F));
  TEST_ASSERT_TRUE(catTxStatusTransmitting(0x00));
  TEST_ASSERT_TRUE(catTxStatusTransmitting(0x1F));
}

void test_command_builders_match_the_opcode_chart() {
  uint8_t c[5];
  const uint8_t on[5] = {0, 0, 0, 0, 0x00}, getf[5] = {0, 0, 0, 0, 0x03},
                gett[5] = {0, 0, 0, 0, 0xF7}, pttOn[5] = {0, 0, 0, 0, 0x08},
                pttOff[5] = {0, 0, 0, 0, 0x88};
  catCmdCatOn(c);        TEST_ASSERT_EQUAL_UINT8_ARRAY(on, c, 5);
  catCmdGetFreqMode(c);  TEST_ASSERT_EQUAL_UINT8_ARRAY(getf, c, 5);
  catCmdGetTxStatus(c);  TEST_ASSERT_EQUAL_UINT8_ARRAY(gett, c, 5);
  catCmdPtt(c, true);    TEST_ASSERT_EQUAL_UINT8_ARRAY(pttOn, c, 5);
  catCmdPtt(c, false);   TEST_ASSERT_EQUAL_UINT8_ARRAY(pttOff, c, 5);
  catCmdSetFreq(c, 432100000);
  const uint8_t sf[5] = {0x43, 0x21, 0x00, 0x00, 0x01};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(sf, c, 5);
}

void test_builders_agree_with_reply_lengths() {
  uint8_t c[5];
  catCmdGetFreqMode(c); TEST_ASSERT_EQUAL(5, catReplyLength(c[4]));
  catCmdGetTxStatus(c); TEST_ASSERT_EQUAL(1, catReplyLength(c[4]));
  catCmdSetMode(c, MODE_AM); TEST_ASSERT_EQUAL(0, catReplyLength(c[4]));
  catCmdPtt(c, true); TEST_ASSERT_EQUAL(0, catReplyLength(c[4]));
  catCmdCatOn(c); TEST_ASSERT_EQUAL(0, catReplyLength(c[4]));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_decode_freq);
  RUN_TEST(test_decode_rejects_non_bcd);
  RUN_TEST(test_encode_freq);
  RUN_TEST(test_freq_roundtrip_and_10hz_rounding);
  RUN_TEST(test_modes_roundtrip_including_narrow);
  RUN_TEST(test_tx_status_polarity);
  RUN_TEST(test_command_builders_match_the_opcode_chart);
  RUN_TEST(test_builders_agree_with_reply_lengths);
  return UNITY_END();
}
