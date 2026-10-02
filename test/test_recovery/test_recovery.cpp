#include <initializer_list>
#include <string.h>
#include <unity.h>

#include "cat_codec.h"
#include "recovery.h"

void setUp() {}

void test_record_roundtrip_for_every_mode_byte() {
  for (int m = 0; m < 256; m++) {
    uint8_t rec[3], out = 0;
    recoveryEncode((uint8_t)m, rec);
    TEST_ASSERT_TRUE(recoveryDecode(rec, out));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)m, out);
  }
}

void test_blank_and_garbage_records_are_not_armed() {
  uint8_t out = 0;
  const uint8_t blank[3] = {0xFF, 0xFF, 0xFF}; // erased EEPROM
  TEST_ASSERT_FALSE(recoveryDecode(blank, out));
  const uint8_t zeros[3] = {0, 0, 0};
  TEST_ASSERT_FALSE(recoveryDecode(zeros, out));
  const uint8_t torn[3] = {RECOVERY_MAGIC, 0x01, 0xFF}; // mode written, complement not yet
  TEST_ASSERT_FALSE(recoveryDecode(torn, out));
  const uint8_t wrongMagic[3] = {0xA6, 0x01, 0xFE};
  TEST_ASSERT_FALSE(recoveryDecode(wrongMagic, out));
}

void test_any_single_corrupted_byte_disarms() {
  uint8_t rec[3];
  recoveryEncode(MODE_AMN, rec);
  for (int i = 0; i < 3; i++) {
    for (uint8_t flip : {0x01, 0x10, 0x80, 0xFF}) {
      uint8_t bad[3];
      memcpy(bad, rec, 3);
      bad[i] ^= flip;
      uint8_t out;
      TEST_ASSERT_FALSE(recoveryDecode(bad, out));
    }
  }
}

void test_script_unkeys_first_restores_mode_and_never_keys() {
  RecoveryStep s[4];
  uint8_t n = recoveryScript(MODE_USB, s);
  TEST_ASSERT_EQUAL(4, n);
  TEST_ASSERT_EQUAL(0x00, s[0].cmd[4]); // CAT on
  TEST_ASSERT_EQUAL(0x88, s[1].cmd[4]); // PTT OFF before anything else changes
  TEST_ASSERT_EQUAL(0x07, s[2].cmd[4]); // restore mode...
  TEST_ASSERT_EQUAL(MODE_USB, s[2].cmd[0]);
  TEST_ASSERT_EQUAL(0x88, s[3].cmd[4]); // ...and PTT off once more
  for (uint8_t i = 0; i < n; i++) {
    TEST_ASSERT_NOT_EQUAL(0x08, s[i].cmd[4]); // never PTT ON
    TEST_ASSERT_GREATER_OR_EQUAL(50, s[i].delayAfterMs); // FT-847 spacing
  }
}

void test_narrow_modes_restore_exactly() {
  RecoveryStep s[4];
  recoveryScript(MODE_CWN, s);
  TEST_ASSERT_EQUAL(MODE_CWN, s[2].cmd[0]);
}

void test_an_unreadable_mode_is_not_written_back() {
  RecoveryStep s[4];
  uint8_t n = recoveryScript(0x55, s); // not an FT-847 mode
  TEST_ASSERT_EQUAL(3, n);
  for (uint8_t i = 0; i < n; i++) TEST_ASSERT_NOT_EQUAL(0x07, s[i].cmd[4]);
  TEST_ASSERT_EQUAL(0x88, s[1].cmd[4]); // but the radio is still unkeyed
  TEST_ASSERT_EQUAL(0x88, s[2].cmd[4]);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_record_roundtrip_for_every_mode_byte);
  RUN_TEST(test_blank_and_garbage_records_are_not_armed);
  RUN_TEST(test_any_single_corrupted_byte_disarms);
  RUN_TEST(test_script_unkeys_first_restores_mode_and_never_keys);
  RUN_TEST(test_narrow_modes_restore_exactly);
  RUN_TEST(test_an_unreadable_mode_is_not_written_back);
  return UNITY_END();
}
