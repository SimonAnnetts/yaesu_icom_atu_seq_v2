#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unity.h>
#include <vector>

#include "config.h"
#include "config_receiver.h"

using Reply = ConfigHost::Reply;

// ---- a host that records everything and checks what it is asked to apply ----

struct Host : ConfigHost {
  bool idle = true;
  bool refuseApply = false; // a band starts transmitting between the handshake and the end
  std::vector<Reply> replies;
  std::vector<std::string> details;
  int applied = 0, saved = 0;
  SequencerConfig lastApplied;
  bool appliedInvalid = false;

  bool idleForUpload() override { return idle; }
  bool apply(const SequencerConfig &cfg) override {
    if (refuseApply) return false;
    const char *why;
    if (!configValidate(cfg, why)) appliedInvalid = true; // must never happen
    lastApplied = cfg;
    applied++;
    return true;
  }
  void save(const SequencerConfig &) override { saved++; }
  void reply(Reply r, const char *detail) override {
    replies.push_back(r);
    details.push_back(detail ? detail : "");
  }
  Reply last() const { return replies.back(); }
  int count(Reply r) const {
    int n = 0;
    for (Reply x : replies) n += x == r;
    return n;
  }
};

static std::string readFile(const char *path) {
  FILE *f = fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string s;
  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

static std::string compact(const std::string &pretty) {
  JsonDocument doc;
  deserializeJson(doc, pretty);
  std::string out;
  serializeJson(doc, out);
  return out;
}

static Host *host;
static ConfigReceiver *rx;
static uint32_t T = 0; // the clock; every character takes 1ms unless a test says otherwise

void setUp() {
  delete rx;
  delete host;
  host = new Host();
  rx = new ConfigReceiver(*host);
  T = 1000;
}

static void feed(const std::string &s, uint32_t gap = 1) {
  for (char c : s) {
    rx->handleChar(c, T);
    T += gap;
  }
}
static void silence(uint32_t ms) { // time passing with nothing on the port, polled every ms
  for (uint32_t i = 0; i < ms; i++) { T++; rx->poll(T); }
}
static void handshake() {
  feed("CONFIG\n");
  TEST_ASSERT_EQUAL(1, host->count(Reply::Ready));
}

static std::string goodConfig() { return readFile("config/sequencer.json"); }

// ---- the handshake ----

void test_a_valid_config_is_accepted_pretty_printed() {
  handshake();
  feed(goodConfig());
  TEST_ASSERT_EQUAL((int)Reply::Ok, (int)host->last());
  TEST_ASSERT_EQUAL(1, host->applied);
  TEST_ASSERT_EQUAL(1, host->saved);
  TEST_ASSERT_FALSE(rx->receiving());
  TEST_ASSERT_FALSE(host->appliedInvalid);
}

void test_a_valid_config_is_accepted_compact_and_with_crlf_everywhere() {
  handshake();
  feed(compact(goodConfig()));
  TEST_ASSERT_EQUAL((int)Reply::Ok, (int)host->last());

  std::string crlf;
  for (char c : goodConfig()) {
    if (c == '\n') crlf += "\r\n";
    else crlf += c;
  }
  feed("CONFIG\r\n");
  feed(crlf);
  TEST_ASSERT_EQUAL(2, host->applied);
}

void test_whitespace_is_not_counted_so_pretty_files_fit() {
  TEST_ASSERT_GREATER_THAN(CONFIG_JSON_MAX, goodConfig().size()); // the shipped file is bigger raw...
  handshake();
  feed(goodConfig());
  TEST_ASSERT_EQUAL((int)Reply::Ok, (int)host->last()); // ...and still fits
}

void test_the_handshake_line_must_be_exactly_CONFIG() {
  const char *notHandshakes[] = {"config\n", "CONFIG \n", " CONFIG\n", "CONFI\n", "CONFIGX\n", "CONFIGCONFIG\n",
                                 "XCONFIG\n", "\n", "\n\n\n", "CON\nFIG\n", "Config\n"};
  for (const char *s : notHandshakes) {
    feed(s);
    char msg[64];
    snprintf(msg, sizeof msg, "'%s' started an upload", s);
    TEST_ASSERT_EQUAL_MESSAGE(0, host->count(Reply::Ready), msg);
    TEST_ASSERT_FALSE(rx->receiving());
  }
}

void test_garbage_before_the_handshake_is_ignored() {
  feed("hello world\r\n");
  feed("ALC: gate ON\n");
  feed(std::string(300, 'x')); // a very long line with no newline...
  feed("\n");                  // ...must not have been mistaken for anything
  handshake();
  feed(goodConfig());
  TEST_ASSERT_EQUAL((int)Reply::Ok, (int)host->last());
}

void test_handshake_characters_are_never_consumed_while_idle() {
  for (char c : std::string("CONFIG\n")) { // even the newline that starts the upload
    TEST_ASSERT_FALSE(rx->handleChar(c, T++));
  }
  TEST_ASSERT_TRUE(rx->receiving()); // ...but everything after it belongs to the upload
}

void test_two_uploads_in_a_row() {
  for (int i = 0; i < 3; i++) {
    handshake_again:
    feed("CONFIG\n");
    feed(compact(goodConfig()));
    TEST_ASSERT_EQUAL((int)Reply::Ok, (int)host->last());
  }
  TEST_ASSERT_EQUAL(3, host->applied);
  TEST_ASSERT_EQUAL(3, host->count(Reply::Ready));
}

void test_upload_split_into_slow_chunks_is_fine() {
  handshake();
  const std::string cfg = compact(goodConfig());
  for (size_t i = 0; i < cfg.size(); i += 40) {
    feed(cfg.substr(i, 40));
    silence(900); // slow, but always inside the 3s timeout
  }
  TEST_ASSERT_EQUAL((int)Reply::Ok, (int)host->last());
}

// ---- damage ----

void test_the_first_character_must_be_an_opening_brace() {
  const char *bad[] = {"[1,2,3]", "42", "\"text\"", "nope", "}", "CONFIG"};
  for (const char *s : bad) {
    silence(CONFIG_LINE_IDLE_MS + 100); // what the rest of a failed upload leaves behind is forgotten
    handshake();
    feed(s);
    char msg[64];
    snprintf(msg, sizeof msg, "'%s'", s);
    TEST_ASSERT_EQUAL_MESSAGE((int)Reply::ExpectedObject, (int)host->last(), msg);
    TEST_ASSERT_FALSE(rx->receiving());
    host->replies.clear();
  }
  TEST_ASSERT_EQUAL(0, host->applied);
}

void test_the_leftover_of_a_failed_upload_does_not_spoil_the_next_handshake() {
  // The sender gives up at the first bad character, but the rest of its compact JSON (no
  // newlines at all) keeps arriving and lands in the idle line buffer.
  handshake();
  feed("x");                      // not '{': ERROR, upload abandoned
  feed(compact(goodConfig()));    // the remainder of the text, still being sent
  TEST_ASSERT_EQUAL((int)Reply::ExpectedObject, (int)host->last());
  silence(1500);                  // the sender retries a little later
  host->replies.clear();
  handshake();
  feed(compact(goodConfig()));
  TEST_ASSERT_EQUAL((int)Reply::Ok, (int)host->last());
  TEST_ASSERT_EQUAL(1, host->applied);
}

void test_a_half_typed_handshake_is_forgotten_after_a_while() {
  feed("CONF");
  silence(CONFIG_LINE_IDLE_MS + 50);
  feed("IG\n"); // would have completed "CONFIG" without the timeout
  TEST_ASSERT_EQUAL(0, host->count(Reply::Ready));
  silence(100);
  feed("CONFIG\n");
  TEST_ASSERT_EQUAL(1, host->count(Reply::Ready));
}

void test_leading_whitespace_before_the_brace_is_fine() {
  handshake();
  feed("\r\n \t\n");
  feed(compact(goodConfig()));
  TEST_ASSERT_EQUAL((int)Reply::Ok, (int)host->last());
}

void test_a_truncated_upload_times_out_and_the_next_one_works() {
  handshake();
  std::string cfg = compact(goodConfig());
  feed(cfg.substr(0, cfg.size() / 2)); // the cable is pulled here
  TEST_ASSERT_TRUE(rx->receiving());
  silence(CONFIG_RX_TIMEOUT_MS - 10);
  TEST_ASSERT_TRUE(rx->receiving());
  silence(20);
  TEST_ASSERT_FALSE(rx->receiving());
  TEST_ASSERT_EQUAL((int)Reply::Timeout, (int)host->last());
  TEST_ASSERT_EQUAL(0, host->applied);

  handshake_second:
  feed("CONFIG\n");
  feed(cfg);
  TEST_ASSERT_EQUAL((int)Reply::Ok, (int)host->last());
  TEST_ASSERT_EQUAL(1, host->applied);
}

void test_ready_then_nothing_times_out() {
  handshake();
  silence(CONFIG_RX_TIMEOUT_MS + 5);
  TEST_ASSERT_EQUAL((int)Reply::Timeout, (int)host->last());
  TEST_ASSERT_FALSE(rx->receiving());
}

void test_a_stalled_idle_port_never_times_out_anything() {
  silence(20000);
  TEST_ASSERT_EQUAL(0, host->replies.size());
}

void test_oversized_upload_is_rejected_at_the_limit() {
  // exactly at the limit: accepted. One character over: rejected.
  JsonDocument doc;
  deserializeJson(doc, goodConfig());
  doc["pad"] = "";
  std::string base;
  serializeJson(doc, base);
  size_t room = CONFIG_JSON_MAX - base.size();
  doc["pad"] = std::string(room, 'p');
  std::string exact;
  serializeJson(doc, exact);
  TEST_ASSERT_EQUAL(CONFIG_JSON_MAX, exact.size());

  handshake();
  feed(exact);
  TEST_ASSERT_EQUAL_MESSAGE((int)Reply::Ok, (int)host->last(), "exactly CONFIG_JSON_MAX characters must be accepted");

  doc["pad"] = std::string(room + 1, 'p');
  std::string over;
  serializeJson(doc, over);
  TEST_ASSERT_EQUAL(CONFIG_JSON_MAX + 1, over.size());
  feed("CONFIG\n");
  feed(over);
  TEST_ASSERT_EQUAL((int)Reply::TooLarge, (int)host->last());
  TEST_ASSERT_FALSE(rx->receiving());
  TEST_ASSERT_EQUAL(1, host->applied);
}

void test_a_huge_upload_is_cut_off_early_not_buffered() {
  handshake();
  std::string big = "{\"x\":\"";
  big += std::string(100000, 'a');
  big += "\"}";
  feed(big, 0); // 100KB at once
  TEST_ASSERT_EQUAL((int)Reply::TooLarge, (int)host->last());
  TEST_ASSERT_FALSE(rx->receiving());
  TEST_ASSERT_EQUAL(1, host->count(Reply::TooLarge)); // one error, not one per extra byte
}

void test_deep_nesting_is_rejected_not_wrapped() {
  // 256 opening braces used to wrap an 8-bit depth counter back to zero
  for (int n : {CONFIG_MAX_DEPTH + 1, 100, 255, 256, 257, 300}) {
    silence(CONFIG_LINE_IDLE_MS + 100);
    handshake();
    feed(std::string(n, '{'));
    char msg[64];
    snprintf(msg, sizeof msg, "%d braces", n);
    TEST_ASSERT_EQUAL_MESSAGE((int)Reply::TooDeep, (int)host->last(), msg);
    TEST_ASSERT_FALSE(rx->receiving());
    host->replies.clear();
  }
  TEST_ASSERT_EQUAL(0, host->applied);
}

void test_brackets_and_braces_inside_strings_do_not_confuse_the_end_detection() {
  JsonDocument doc;
  deserializeJson(doc, goodConfig());
  doc["note"] = "}{ ]] [[ \" \\ }}}}";
  std::string text;
  serializeJson(doc, text);
  handshake();
  feed(text);
  TEST_ASSERT_EQUAL((int)Reply::Ok, (int)host->last());
  TEST_ASSERT_EQUAL(1, host->applied);
}

void test_json_syntax_errors_are_reported_and_nothing_is_applied() {
  const char *bad[] = {"{a:b}", "{\"schema_version\": 1,}", "{\"a\": }", "{\"a\" 1}", "{]"};
  for (const char *s : bad) {
    handshake();
    feed(s);
    char msg[64];
    snprintf(msg, sizeof msg, "'%s'", s);
    TEST_ASSERT_EQUAL_MESSAGE((int)Reply::InvalidJson, (int)host->last(), msg);
    host->replies.clear();
  }
  TEST_ASSERT_EQUAL(0, host->applied);
  TEST_ASSERT_EQUAL(0, host->saved);
}

void test_valid_json_that_is_not_a_config_is_refused_with_the_reason() {
  handshake();
  feed("{\"schema_version\":2,\"bands\":{}}");
  TEST_ASSERT_EQUAL((int)Reply::InvalidConfig, (int)host->last());
  TEST_ASSERT_NOT_NULL(strstr(host->details.back().c_str(), "schema_version"));
  handshake_two:
  feed("CONFIG\n");
  feed("{}");
  TEST_ASSERT_EQUAL((int)Reply::InvalidConfig, (int)host->last());
  TEST_ASSERT_EQUAL(0, host->applied);
}

void test_a_config_with_overlapping_bands_is_never_applied() {
  JsonDocument doc;
  deserializeJson(doc, goodConfig());
  doc["bands"]["HF"]["freq_max_hz"] = 50000000;
  std::string text;
  serializeJson(doc, text);
  handshake();
  feed(text);
  TEST_ASSERT_EQUAL((int)Reply::InvalidConfig, (int)host->last());
  TEST_ASSERT_EQUAL(0, host->applied);
  TEST_ASSERT_EQUAL(0, host->saved);
}

// ---- busy ----

void test_refused_up_front_while_a_band_is_transmitting() {
  host->idle = false;
  feed("CONFIG\n");
  TEST_ASSERT_EQUAL((int)Reply::Busy, (int)host->last());
  TEST_ASSERT_EQUAL(0, host->count(Reply::Ready)); // no READY: the sender isn't invited to send
  TEST_ASSERT_FALSE(rx->receiving());
  // and the JSON that a careless sender sends anyway is just noise, not an upload
  feed(goodConfig());
  TEST_ASSERT_EQUAL(0, host->applied);
}

void test_refused_at_the_end_if_a_band_started_meanwhile() {
  handshake();
  const std::string cfg = compact(goodConfig());
  feed(cfg.substr(0, cfg.size() - 1));
  host->refuseApply = true; // PTT pressed during the upload
  feed(cfg.substr(cfg.size() - 1));
  TEST_ASSERT_EQUAL((int)Reply::Busy, (int)host->last());
  TEST_ASSERT_EQUAL(0, host->applied);
  TEST_ASSERT_EQUAL(0, host->saved); // and nothing was written to EEPROM
}

void test_a_refusal_leaves_the_receiver_ready_for_the_next_try() {
  host->idle = false;
  feed("CONFIG\n");
  host->idle = true;
  feed("CONFIG\n");
  feed(compact(goodConfig()));
  TEST_ASSERT_EQUAL((int)Reply::Ok, (int)host->last());
}

// ---- noise and fuzzing ----

static uint32_t rng = 12345;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

void test_random_bytes_never_corrupt_the_receiver_or_apply_anything_invalid() {
  // a mix weighted towards the characters that drive the state machine
  const char alphabet[] = "{}[]\"\\: ,\n\r\tCONFIG0123456789abcdefxyz_";
  for (int round = 0; round < 400; round++) {
    delete rx;
    delete host;
    host = new Host();
    rx = new ConfigReceiver(*host);
    T = 1000;
    int len = rnd() % 3000;
    for (int i = 0; i < len; i++) {
      char c = (rnd() % 8 == 0) ? (char)(rnd() & 0xFF) : alphabet[rnd() % (sizeof alphabet - 1)];
      rx->handleChar(c, T);
      T += rnd() % 3;
      if (rnd() % 997 == 0) { // and sometimes, a pause
        silence(rnd() % 4000);
      }
      if (rnd() % 499 == 0) feed("CONFIG\n"); // keep restarting uploads in the middle of anything
    }
    silence(CONFIG_RX_TIMEOUT_MS + 10);
    TEST_ASSERT_FALSE_MESSAGE(rx->receiving(), "receiver stuck after the port went quiet");
    TEST_ASSERT_FALSE_MESSAGE(host->appliedInvalid, "an invalid config was applied");
    // every apply was matched by exactly one save and one OK
    TEST_ASSERT_EQUAL(host->applied, host->saved);
    TEST_ASSERT_EQUAL(host->applied, host->count(Reply::Ok));
  }
}

void test_a_valid_config_survives_random_whitespace_and_random_chunking() {
  const std::string text = compact(goodConfig());
  JsonDocument expect;
  deserializeJson(expect, text);
  for (int round = 0; round < 300; round++) {
    delete rx;
    delete host;
    host = new Host();
    rx = new ConfigReceiver(*host);
    T = 1000;
    // insert random whitespace between tokens (never inside a string)
    std::string noisy;
    bool inStr = false, esc = false;
    for (char c : text) {
      noisy += c;
      if (inStr) {
        if (esc) esc = false;
        else if (c == '\\') esc = true;
        else if (c == '"') inStr = false;
      } else if (c == '"') {
        inStr = true;
      } else if (c == ',' || c == ':' || c == '{' || c == '}') {
        int n = rnd() % 4;
        for (int i = 0; i < n; i++) noisy += " \t\r\n"[rnd() % 4];
      }
    }
    feed("CONFIG\n");
    for (char c : noisy) {
      rx->handleChar(c, T);
      T += 1 + rnd() % 5;
      if (rnd() % 300 == 0) silence(rnd() % 1500); // always under the timeout
    }
    char msg[64];
    snprintf(msg, sizeof msg, "round %d", round);
    TEST_ASSERT_EQUAL_MESSAGE((int)Reply::Ok, (int)host->last(), msg);
    TEST_ASSERT_EQUAL_MESSAGE(1, host->applied, msg);
    TEST_ASSERT_EQUAL_UINT32(1800000, host->lastApplied.band[0].freqMinHz);
  }
}

void test_millis_wraparound_during_an_upload() {
  T = 0xFFFFFF00u;
  handshake();
  feed(compact(goodConfig()), 2); // the clock wraps part-way through
  TEST_ASSERT_EQUAL((int)Reply::Ok, (int)host->last());

  feed("CONFIG\n");
  feed("{\"a\":1");
  silence(CONFIG_RX_TIMEOUT_MS + 5);
  TEST_ASSERT_EQUAL((int)Reply::Timeout, (int)host->last());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_valid_config_is_accepted_pretty_printed);
  RUN_TEST(test_a_valid_config_is_accepted_compact_and_with_crlf_everywhere);
  RUN_TEST(test_whitespace_is_not_counted_so_pretty_files_fit);
  RUN_TEST(test_the_handshake_line_must_be_exactly_CONFIG);
  RUN_TEST(test_garbage_before_the_handshake_is_ignored);
  RUN_TEST(test_handshake_characters_are_never_consumed_while_idle);
  RUN_TEST(test_two_uploads_in_a_row);
  RUN_TEST(test_upload_split_into_slow_chunks_is_fine);
  RUN_TEST(test_the_first_character_must_be_an_opening_brace);
  RUN_TEST(test_the_leftover_of_a_failed_upload_does_not_spoil_the_next_handshake);
  RUN_TEST(test_a_half_typed_handshake_is_forgotten_after_a_while);
  RUN_TEST(test_leading_whitespace_before_the_brace_is_fine);
  RUN_TEST(test_a_truncated_upload_times_out_and_the_next_one_works);
  RUN_TEST(test_ready_then_nothing_times_out);
  RUN_TEST(test_a_stalled_idle_port_never_times_out_anything);
  RUN_TEST(test_oversized_upload_is_rejected_at_the_limit);
  RUN_TEST(test_a_huge_upload_is_cut_off_early_not_buffered);
  RUN_TEST(test_deep_nesting_is_rejected_not_wrapped);
  RUN_TEST(test_brackets_and_braces_inside_strings_do_not_confuse_the_end_detection);
  RUN_TEST(test_json_syntax_errors_are_reported_and_nothing_is_applied);
  RUN_TEST(test_valid_json_that_is_not_a_config_is_refused_with_the_reason);
  RUN_TEST(test_a_config_with_overlapping_bands_is_never_applied);
  RUN_TEST(test_refused_up_front_while_a_band_is_transmitting);
  RUN_TEST(test_refused_at_the_end_if_a_band_started_meanwhile);
  RUN_TEST(test_a_refusal_leaves_the_receiver_ready_for_the_next_try);
  RUN_TEST(test_random_bytes_never_corrupt_the_receiver_or_apply_anything_invalid);
  RUN_TEST(test_a_valid_config_survives_random_whitespace_and_random_chunking);
  RUN_TEST(test_millis_wraparound_during_an_upload);
  return UNITY_END();
}
