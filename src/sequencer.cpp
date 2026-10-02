#include "sequencer.h"

const SequencerConfig DEFAULT_SEQUENCER_CONFIG = {
    {
        // Band edges are the placeholder IARU Region 1 values.
        // HF: tune profile skips SEQ3
        // A tuner like the AH-4 covers 160-6m, so the ATU is on for HF and 50M only.
        {1800000, 29700000, {50, 50, 20}, {true, true, false}, true, true},
        {50000000, 54000000, {50, 50, 20}, {true, true, true}, true, true},   // 50M
        {144000000, 146000000, {50, 50, 20}, {true, true, true}, false, true}, // 144M
        {430000000, 440000000, {50, 50, 20}, {true, true, true}, false, true}, // 430M
    },
    0,
    {},
};

int8_t bandForFrequency(const SequencerConfig &cfg, uint32_t hz) {
  for (uint8_t b = 0; b < SEQ_BANDS; b++) {
    if (hz >= cfg.band[b].freqMinHz && hz <= cfg.band[b].freqMaxHz) return b;
  }
  return -1;
}

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

bool Sequencer::allIdle() const {
  for (uint8_t b = 0; b < SEQ_BANDS; b++) {
    if (band_[b].state != State::Idle) return false;
  }
  return true;
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
  // Cross-band triggers follow the source band's SEQ1 output. They run after the
  // loop so a rule's source reflects that band's own sequence only.
  for (uint8_t i = 0; i < cfg_->triggerCount; i++) {
    const CrossBandTrigger &t = cfg_->trigger[i];
    if (band_[t.sourceBand].on[0]) o.seq[t.targetBand][t.targetStage] = true;
  }
  return o;
}
