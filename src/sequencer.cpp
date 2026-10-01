#include "sequencer.h"

constexpr uint16_t GAP_MS = 300;

const SequencerConfig DEFAULT_SEQUENCER_CONFIG = {{
    // HF: tune profile skips SEQ3
    {{GAP_MS, GAP_MS, GAP_MS}, {true, true, false}},
    // 50M, 144M, 430M
    {{GAP_MS, GAP_MS, GAP_MS}, {true, true, true}},
    {{GAP_MS, GAP_MS, GAP_MS}, {true, true, true}},
    {{GAP_MS, GAP_MS, GAP_MS}, {true, true, true}},
}};

static bool reached(uint32_t now, uint32_t dueAt) {
  return (int32_t)(now - dueAt) >= 0; // wrap-safe
}

bool SequencerOutputs::operator==(const SequencerOutputs &o) const {
  if (txInhibit != o.txInhibit) return false;
  for (uint8_t b = 0; b < SEQ_BANDS; b++) {
    if (rx[b] != o.rx[b] || tx[b] != o.tx[b]) return false;
    for (uint8_t s = 0; s < SEQ_STAGES; s++) {
      if (seq[b][s] != o.seq[b][s]) return false;
    }
  }
  return true;
}

bool Sequencer::included(uint8_t band, uint8_t stage) const {
  return band_[band].profile == Profile::Normal || cfg_->band[band].tuneProfile[stage];
}

// Highest stage currently on, or -1.
int8_t Sequencer::highestOn(uint8_t band) const {
  for (int8_t s = SEQ_STAGES - 1; s >= 0; s--) {
    if (band_[band].on[s]) return s;
  }
  return -1;
}

void Sequencer::startDown(uint8_t band, uint32_t now) {
  Band &b = band_[band];
  b.state = State::Down;
  b.txLed = false; // TX off immediately; this also releases this band's inhibit
  int8_t top = highestOn(band);
  if (top < 0) {
    b.state = State::Idle;
  } else {
    b.dueAt = now + cfg_->band[band].gapMs[top];
  }
}

void Sequencer::request(uint8_t band, bool wantTx, Profile profile, uint32_t now) {
  if (band >= SEQ_BANDS) return;
  Band &b = band_[band];
  if (wantTx) {
    if (b.state == State::Up || b.state == State::Active) return; // idempotent
    b.profile = profile;
    b.state = State::Up; // from Idle, or turning round mid-down: keep stages already on
    int8_t top = highestOn(band);
    b.dueAt = top < 0 ? now : now + cfg_->band[band].gapMs[top];
  } else {
    if (b.state == State::Idle || b.state == State::Down) return;
    startDown(band, now);
  }
}

void Sequencer::stby(uint8_t band, bool asserted, uint32_t now) {
  if (band >= SEQ_BANDS || band_[band].held) return;
  request(band, asserted, Profile::Normal, now);
}

void Sequencer::pollBand(uint8_t band, uint32_t now) {
  Band &b = band_[band];

  if (b.state == State::Up) {
    while (b.state == State::Up && reached(now, b.dueAt)) {
      int8_t next = -1;
      for (uint8_t s = 0; s < SEQ_STAGES; s++) {
        if (included(band, s) && !b.on[s]) {
          next = s;
          break;
        }
      }
      if (next < 0) {
        b.txLed = true; // every engaged stage settled: clear to transmit
        b.state = State::Active;
      } else {
        b.on[next] = true;
        b.dueAt += cfg_->band[band].gapMs[next]; // chained from due time: no drift
      }
    }
  } else if (b.state == State::Down) {
    while (b.state == State::Down && reached(now, b.dueAt)) {
      int8_t top = highestOn(band);
      if (top >= 0) b.on[top] = false;
      top = highestOn(band);
      if (top < 0) {
        b.state = State::Idle;
      } else {
        b.dueAt += cfg_->band[band].gapMs[top];
      }
    }
  }
}

void Sequencer::poll(uint32_t now) {
  for (uint8_t b = 0; b < SEQ_BANDS; b++) pollBand(b, now);
}

SequencerOutputs Sequencer::outputs() const {
  SequencerOutputs o = {};
  for (uint8_t b = 0; b < SEQ_BANDS; b++) {
    const Band &st = band_[b];
    o.rx[b] = st.state == State::Idle;
    o.tx[b] = st.txLed;
    for (uint8_t s = 0; s < SEQ_STAGES; s++) o.seq[b][s] = st.on[s];
    if (st.state == State::Up) o.txInhibit = true;
  }
  return o;
}
