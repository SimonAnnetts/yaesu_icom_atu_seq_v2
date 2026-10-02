#pragma once

#include <stdint.h>

// Pure amplifier-sequencer logic: no Arduino, no pins, time passed in. See
// README "Sequencer" for behaviour. Band index matches pins.h `Band`.
//
// Per band, outputs are cumulative: SEQ1..SEQ3 latch on in order and stay on;
// RX is lit exactly while the band is idle; TX lights once every engaged stage
// has settled. TX INHIBIT is shared and is held while any band is mid
// up-sequence.

constexpr uint8_t SEQ_BANDS = 4;
constexpr uint8_t SEQ_STAGES = 3; // SEQ1..SEQ3

struct BandConfig {
  uint32_t freqMinHz; // band edges, used to map the radio's frequency to a band
  uint32_t freqMaxHz;
  // Delay after stage s (index 0 = SEQ1) has been engaged before the next step
  // (SEQ2, SEQ3, then TX). The down-sequence mirrors these: the same delay is
  // waited before dropping stage s.
  uint16_t gapMs[SEQ_STAGES];  // seq1_to_seq2, seq2_to_seq3, seq3_to_tx
  bool tuneProfile[SEQ_STAGES]; // stage engaged during a tune cycle?
  bool atu;                     // may the tune cycle run on this band? (the tuner's coverage)
  bool alc;                     // inject ALC (pump voltage on the radio's ALC line) when tuning on this band
};

// Cross-band trigger: while the source band's SEQ1 is on, the target band's
// output `targetStage` (0 = SEQ1 .. 2 = SEQ3) is forced on, snapping on and off
// with it. Rules only ever turn outputs on: several rules aimed at the same
// output, or an output the target band's own sequence already has on, just OR.
struct CrossBandTrigger {
  uint8_t sourceBand;
  uint8_t targetBand;
  uint8_t targetStage;
};
constexpr uint8_t MAX_TRIGGERS = 8;

struct SequencerConfig {
  BandConfig band[SEQ_BANDS];
  uint8_t triggerCount;
  CrossBandTrigger trigger[MAX_TRIGGERS];
};

// Built-in fallback, used when EEPROM is empty or invalid: the band edges, timing
// and tune profiles of config/sequencer.json (a native test keeps them in
// step), with no cross-band triggers.
extern const SequencerConfig DEFAULT_SEQUENCER_CONFIG;

// Index of the band containing hz, or -1 if it is in none of them.
int8_t bandForFrequency(const SequencerConfig &cfg, uint32_t hz);

enum class Profile : uint8_t { Normal, Tune };

struct SequencerOutputs {
  bool rx[SEQ_BANDS];
  bool seq[SEQ_BANDS][SEQ_STAGES];
  bool tx[SEQ_BANDS];
  bool txInhibit;

  bool operator==(const SequencerOutputs &o) const;
};

class Sequencer {
public:
  explicit Sequencer(const SequencerConfig &cfg) : cfg_(&cfg) {}

  // STBY-driven request (normal profile). Ignored while the band is held.
  void stby(uint8_t band, bool asserted, uint32_t now);

  // Explicit request, e.g. from the tune cycle. Not affected by hold.
  void request(uint8_t band, bool wantTx, Profile profile, uint32_t now);

  // While held, stby() for that band is ignored (tune cycle owns the band).
  void setHold(uint8_t band, bool held) { band_[band].held = held; }

  // Advance timers; call every loop.
  void poll(uint32_t now);

  SequencerOutputs outputs() const;

  bool allIdle() const;
  bool idle(uint8_t band) const { return band_[band].state == State::Idle; }
  bool active(uint8_t band) const { return band_[band].state == State::Active; }

private:
  enum class State : uint8_t { Idle, Up, Active, Down };

  struct Band {
    State state = State::Idle;
    Profile profile = Profile::Normal;
    bool on[SEQ_STAGES] = {};
    bool txLed = false;
    bool held = false;
    uint32_t dueAt = 0;
  };

  bool included(uint8_t band, uint8_t stage) const;
  int8_t highestOn(uint8_t band) const;
  void startDown(uint8_t band, uint32_t now);
  void pollBand(uint8_t band, uint32_t now);

  const SequencerConfig *cfg_;
  Band band_[SEQ_BANDS];
};
