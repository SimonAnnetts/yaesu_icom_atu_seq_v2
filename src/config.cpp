#include "config.h"

#include <string.h>

bool configValidate(const SequencerConfig &cfg, const char *&err) {
  for (uint8_t b = 0; b < SEQ_BANDS; b++) {
    const BandConfig &band = cfg.band[b];
    if (band.freqMinHz >= band.freqMaxHz) {
      err = "band edges: freq_min_hz must be below freq_max_hz";
      return false;
    }
    for (uint8_t g = 0; g < SEQ_STAGES; g++) {
      if (band.gapMs[g] > CONFIG_MAX_GAP_MS) {
        err = "timing_ms value above 5000";
        return false;
      }
    }
    for (uint8_t o = b + 1; o < SEQ_BANDS; o++) {
      const BandConfig &other = cfg.band[o];
      if (band.freqMinHz <= other.freqMaxHz && other.freqMinHz <= band.freqMaxHz) {
        err = "band frequency ranges overlap";
        return false;
      }
    }
  }
  if (cfg.triggerCount > MAX_TRIGGERS) {
    err = "too many cross_band_triggers";
    return false;
  }
  for (uint8_t i = 0; i < cfg.triggerCount; i++) {
    const CrossBandTrigger &t = cfg.trigger[i];
    if (t.sourceBand >= SEQ_BANDS || t.targetBand >= SEQ_BANDS || t.targetStage >= SEQ_STAGES) {
      err = "cross_band_triggers entry out of range";
      return false;
    }
    if (t.sourceBand == t.targetBand) {
      err = "cross_band_triggers source and target are the same band";
      return false;
    }
  }
  return true;
}

uint16_t configCrc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
  }
  return crc;
}

static const uint8_t MAGIC[4] = {'A', 'T', 'U', 'S'};
constexpr uint8_t LAYOUT_VERSION = 1;
constexpr size_t HEADER_LEN = 4 + 1 + 2; // magic, version, payload length

namespace {
struct Writer {
  uint8_t *buf;
  size_t cap;
  size_t n;
  bool ok;
  Writer(uint8_t *b, size_t c) : buf(b), cap(c), n(0), ok(true) {}
  void u8(uint8_t v) {
    if (n < cap) buf[n++] = v;
    else ok = false;
  }
  void u16(uint16_t v) { u8(v & 0xFF); u8(v >> 8); }
  void u32(uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); }
};

struct Reader {
  const uint8_t *buf;
  size_t len;
  size_t n;
  bool ok;
  Reader(const uint8_t *b, size_t l) : buf(b), len(l), n(0), ok(true) {}
  uint8_t u8() {
    if (n >= len) { ok = false; return 0; }
    return buf[n++];
  }
  uint16_t u16() { uint16_t lo = u8(); return lo | (uint16_t)u8() << 8; }
  uint32_t u32() { uint32_t lo = u16(); return lo | (uint32_t)u16() << 16; }
};
} // namespace

size_t configSerialize(const SequencerConfig &cfg, uint8_t *buf, size_t cap) {
  Writer w(buf, cap);
  for (uint8_t i = 0; i < 4; i++) w.u8(MAGIC[i]);
  w.u8(LAYOUT_VERSION);
  w.u16(0); // payload length, patched below
  for (uint8_t b = 0; b < SEQ_BANDS; b++) {
    const BandConfig &band = cfg.band[b];
    w.u32(band.freqMinHz);
    w.u32(band.freqMaxHz);
    for (uint8_t g = 0; g < SEQ_STAGES; g++) w.u16(band.gapMs[g]);
    for (uint8_t s = 0; s < SEQ_STAGES; s++) w.u8(band.tuneProfile[s] ? 1 : 0);
  }
  w.u8(cfg.triggerCount);
  for (uint8_t i = 0; i < cfg.triggerCount && i < MAX_TRIGGERS; i++) {
    w.u8(cfg.trigger[i].sourceBand);
    w.u8(cfg.trigger[i].targetBand);
    w.u8(cfg.trigger[i].targetStage);
  }
  for (uint8_t b = 0; b < SEQ_BANDS; b++) w.u8(cfg.band[b].atu ? 1 : 0); // added after v1 images existed
  if (!w.ok) return 0;
  size_t payload = w.n - HEADER_LEN;
  buf[5] = payload & 0xFF;
  buf[6] = payload >> 8;
  uint16_t crc = configCrc16(buf, w.n);
  w.u16(crc);
  return w.ok ? w.n : 0;
}

bool configDeserialize(const uint8_t *buf, size_t len, SequencerConfig &out, const char *&err) {
  if (len < HEADER_LEN + 2) {
    err = "image too short";
    return false;
  }
  if (memcmp(buf, MAGIC, 4) != 0) {
    err = "bad magic";
    return false;
  }
  if (buf[4] != LAYOUT_VERSION) {
    err = "unsupported layout version";
    return false;
  }
  size_t payload = buf[5] | (size_t)buf[6] << 8;
  if (HEADER_LEN + payload + 2 > len) {
    err = "length field exceeds image";
    return false;
  }
  size_t crcAt = HEADER_LEN + payload;
  uint16_t stored = buf[crcAt] | (uint16_t)buf[crcAt + 1] << 8;
  if (configCrc16(buf, crcAt) != stored) {
    err = "CRC mismatch";
    return false;
  }

  SequencerConfig cfg = {};
  Reader r(buf + HEADER_LEN, payload);
  for (uint8_t b = 0; b < SEQ_BANDS; b++) {
    BandConfig &band = cfg.band[b];
    band.freqMinHz = r.u32();
    band.freqMaxHz = r.u32();
    for (uint8_t g = 0; g < SEQ_STAGES; g++) band.gapMs[g] = r.u16();
    for (uint8_t s = 0; s < SEQ_STAGES; s++) band.tuneProfile[s] = r.u8() != 0;
  }
  cfg.triggerCount = r.u8();
  if (cfg.triggerCount > MAX_TRIGGERS) {
    err = "too many cross_band_triggers";
    return false;
  }
  for (uint8_t i = 0; i < cfg.triggerCount; i++) {
    cfg.trigger[i].sourceBand = r.u8();
    cfg.trigger[i].targetBand = r.u8();
    cfg.trigger[i].targetStage = r.u8();
  }
  // Images written before the per-band ATU flag existed end here: keep the old
  // behaviour (HF and 50M on) for them.
  size_t left = payload - r.n;
  if (left != 0 && left != SEQ_BANDS) {
    err = "payload size does not match layout";
    return false;
  }
  for (uint8_t b = 0; b < SEQ_BANDS; b++) {
    cfg.band[b].atu = left ? r.u8() != 0 : b <= 1;
  }
  if (!r.ok || r.n != payload) {
    err = "payload size does not match layout";
    return false;
  }
  if (!configValidate(cfg, err)) return false;
  out = cfg;
  return true;
}
