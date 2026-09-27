# Yaesu/Icom ATU Sequencer v2

An Arduino Mega 2560 project that sits transparently between a PC running CAT
control software (e.g. `flrig`) and a Yaesu FT-847 transceiver, so it can
automatically sequence an Icom automatic antenna tuner (e.g. AH-4) without the
PC being aware that anything happened.

> `docs/Hamlib` and `docs/fakeFC` are local clones kept only for reference
> while researching the FT-847 CAT protocol and Icom AH-4 timing (see the
> reference sections below) — they're excluded via `.gitignore` and are not
> part of this repo. Their upstream sources are
> [Hamlib/Hamlib](https://github.com/Hamlib/Hamlib) and
> [doumae/fakeFC](https://github.com/doumae/fakeFC).

## Goal

The PC believes it is talking directly to the radio's CAT port. In reality it
is talking to the Arduino, which brokers the conversation:

- Under normal conditions, CAT traffic is passed through transparently
  between the PC and the radio in both directions.
- The Arduino can intercept and, when needed, modify traffic between the two,
  or talk to the radio on its own initiative without the PC's knowledge.
- When a physical tune button is pressed, the Arduino runs a self-contained
  tune sequence: it drives the Icom ATU interface, temporarily takes over CAT
  control of the radio to set it up for tuning, and then restores the radio
  to its prior state — all invisibly to the PC.
- **The PC is optional.** With no PC connected at all, the tune button and
  ATU sequencing must still work exactly the same way — the Arduino talks
  to the radio directly over Port 2 regardless of what is or isn't
  happening on Port 1. Standalone operation, not just PC-transparent
  operation, is a first-class requirement.
- Independently of CAT and the ATU, the Arduino also runs a **4-band
  amplifier sequencer** (HF/50/144/430MHz), driven from the radio's STBY
  jack — see "Sequencer" under Behaviour below.

## Hardware

- **Arduino Mega 2560** as the controller.
- **Serial0 (USB)**: connected to a PC via USB, used for debugging/logging and
  local control/monitoring of the Arduino itself. Not part of the CAT path.
- **Serial2 ("Port 1")**: connects to the PC's CAT serial port, via one half
  of a MAX202CPE (RS-232 level shifter).
- **Serial3 ("Port 2")**: connects to the Yaesu FT-847's CAT port, via the
  other half of the MAX202CPE.
- **Serial1**: unused, left entirely free — see pin plan below.
- **Icom ATU interface**: drives an Icom-series automatic antenna tuner (e.g.
  AH-4) using its start/key line(s) and tune-complete signalling.
- **Tune button**: a physical pushbutton wired to an Arduino input, used to
  initiate a tune cycle.
- **ALC injection circuit**: a charge pump driven from an Arduino PWM pin
  (producing roughly -4V), gated onto the radio's ALC line through an
  opto-isolator used purely as a switch. This lets the Arduino pull the
  radio's RF output down closer to true minimum during a tune cycle,
  independent of — and in addition to — the AM-mode power ceiling (see
  below). Carried over from a prior project; confirmed against the FT-847
  manual — see the ALC reference section further down.
- **Amplifier sequencer**: 4 independent relay sequencers (one per band:
  HF, 50, 144, 430MHz — see the open question below on which physical
  band maps to which "Band 1-4" channel), each with 5 distinct outputs —
  `RX`, `SEQ1`, `SEQ2`, `SEQ3`, `TX` — for exactly one of which is driven
  active at a time. All 5 drive a status LED; only `SEQ1`/`SEQ2`/`SEQ3`
  also drive an opto-isolator (`RX`/`TX` are LED indicators only, no
  relay/opto function) — 12 opto-isolator outputs total, 3 per band (see
  Pin plan). Triggered by the radio's STBY jack (4 closure-to-ground T/R
  lines, wired
  to the interrupt-capable pins — see Pin plan). A single, shared **TX
  INHIBIT** output (D4) holds off the radio's actual transmit output while
  each band's sequence runs, wired to pin 8 of the radio's TUNER
  connector. See "Sequencer" under Behaviour, and the TUNER-port reference
  section further down for the confirmed pinout and how the CAT-disable
  risk is avoided (by deliberately leaving the TUNER connector's separate
  Tuner Sense pin unconnected).

### Pin plan (Mega 2560)

| Pin(s) | Function |
|---|---|
| D0/D1 (Serial0) | USB debug/control link to PC |
| D16/D17 (Serial2) | Port 1 — CAT to PC, via MAX202CPE |
| D14/D15 (Serial3) | Port 2 — CAT to radio, via MAX202CPE |
| D7 | Tune button (input, polled) |
| D8 | Tune status LED (output) |
| D9 | ALC injection charge pump (PWM output) |
| D10 | ALC opto-isolator gate (on/off switch) |
| D11 | Icom AH-4 `KEY` input |
| D12 | Icom AH-4 `START` output |
| D2 | STBY: HF (input, interrupt) |
| D3 | STBY: 430MHz (input, interrupt) |
| D18 | STBY: 144MHz (input, interrupt) |
| D19 | STBY: 50MHz (input, interrupt) |
| D4 | Shared TX INHIBIT output → TUNER connector pin 8 |
| D5 | TUNER connector pin 2 (`TX_GND`, per fakeFC's schematic) — wired in as an input, matching fakeFC's own pin choice; purpose/use TBD |
| D22 / D23 / D24 / D25 / D26 | Sequencer Band 1: `RX` (LED only) / `SEQ1` (LED+opto) / `SEQ2` (LED+opto) / `SEQ3` (LED+opto) / `TX` (LED only) |
| D27 / D28 / D29 / D30 / D31 | Sequencer Band 2: `RX` / `SEQ1` / `SEQ2` / `SEQ3` / `TX` — same LED-only/LED+opto pattern as Band 1 |
| D44 / D45 / D46 / D47 / D48 | Sequencer Band 3: `RX` / `SEQ1` / `SEQ2` / `SEQ3` / `TX` — same pattern |
| D49 / D50 / D51 / D52 / D53 | Sequencer Band 4: `RX` / `SEQ1` / `SEQ2` / `SEQ3` / `TX` — same pattern; D50-D53 are the Mega's hardware SPI pins (MISO/MOSI/SCK/SS), repurposed as plain digital outputs since SPI isn't needed elsewhere in this project |
| D20, D21 (I2C: SDA/SCL) | **kept free** — not used by anything, available for a future I2C peripheral (e.g. a status display) |

Sequencer channels are numbered "Band 1-4" for now rather than tied to a
specific RF band — see the open question below on mapping each channel to
HF/50/144/430MHz and cross-referencing against the STBY pin assignments
above.

Only six Mega 2560 pins support true external interrupts: D2, D3, D18,
D19, D20, D21. Putting both CAT links on Serial2/Serial3 (rather than
Serial1) means Serial1's pins — D18/D19, which are two of those six
interrupt-capable pins — are never touched, so **all six** interrupt pins
stay free instead of just four. Four of them (D2, D3, D18, D19) are now
used for the 4 STBY band lines (interrupt-driven, since a TX request
needs to be caught immediately to start the sequencer); the remaining two
(D20/D21) stay free of any reservation for I2C. The tune button (D7)
doesn't need an interrupt-capable pin — it's polled in the main loop — so
it and the status LED (D8) sit comfortably outside the reserved set. The
12 sequencer outputs and the shared TX INHIBIT output (D22-D34) are plain
digital outputs with no special pin requirements, so their exact pin
numbers are a free choice — the assignment above is a proposal, not yet
confirmed. The Arduino-side band↔pin mapping for STBY (which of D2/D3/D18/D19
is HF vs 50 vs 144 vs 430) is likewise our own software's choice; what
actually matters is wiring it consistently against the STBY jack's real
per-band wiring (see the STBY jack reference below).

## Behaviour

### Normal operation (passthrough)

- Bytes arriving on Port 1 (from the PC) are forwarded to Port 2 (to the
  radio), and bytes arriving on Port 2 (from the radio) are forwarded to
  Port 1 (to the PC).
- If no PC is connected, Port 1 simply carries no traffic — there is
  nothing to relay. This is a normal, fully supported state, not an error
  condition; see "Standalone operation" below for how the tune cycle still
  works correctly in this case.

### What actually needs faking during a tune cycle

The tune cycle only ever changes two things about the radio's externally
visible state: **mode** (forced to AM) and **PTT/TX status** (forced on).
Frequency doesn't change; CTCSS/DCS/satellite-toggle/etc. don't change;
none of it is touched by tuning. That means the Arduino only ever needs to
fake two specific things, not maintain a general-purpose cache of
"everything the PC might ask":

- **Mode** — the mode byte within the `Get freq+mode, Main` reply
  (opcode `0x03`; see the CAT reference below). This reply packs
  frequency and mode into one 5-byte frame, so the Arduino must splice the
  *real, live* frequency (queried from the radio, since it's genuinely
  unchanged) together with the *faked* pre-tune mode byte — not fake the
  whole response.
- **PTT/TX status** — the PTT bit in the `Get TX status` reply
  (opcode `0xF7`).
- Nothing else needs faking. In particular, RX status/S-meter (opcode
  `0xE7`) can be forwarded live and truthfully even while keyed — a
  receiver being blind during any transmission (including an entirely
  ordinary, manually-initiated one) is unremarkable and gives nothing
  away.
- **Any CAT query the Arduino doesn't specifically recognise is simply
  forwarded to the radio and its response forwarded back, live, even
  during a tune cycle.** There is no cache-completeness problem to solve —
  a query the Arduino has never seen before isn't one it needs to fake an
  answer for, because the radio itself can truthfully answer it regardless
  of whether it's mid-tune. Pre-populating an exhaustive cache of "every
  possible CAT parameter" is unnecessary; only the two items above ever
  diverge from the truth.

### Tune cycle (triggered by the tune button)

1. Actively query the radio's current mode over CAT (`Get freq+mode, Main`,
   opcode `0x03`) and note the current PTT/TX status is "off" (this is
   always known without needing to ask, since the Arduino is the one about
   to turn it on). This captures the two values that will need to be
   faked and restored, freshly, at the start of every cycle — so it works
   identically whether or not a PC (and any snooped history) exists.
2. Issue the appropriate start sequence to the Icom ATU interface.
3. Take over the CAT link to the radio and:
   - Select AM mode (there is no CAT power-set command on this radio —
     switching to AM mode is itself the main power reduction, since the
     FT-847 caps AM output much lower than SSB/CW/FM; see the CAT
     reference below).
   - Optionally assert the ALC injection circuit to pull power down
     further, closer to true minimum, independent of CAT.
   - Key the radio's PTT.
   - Meanwhile, continue forwarding any other CAT traffic between PC and
     radio live (see above) — only mode and PTT/TX status queries get
     answered from the values captured in step 1 instead of the truth.
4. Wait for the ATU to signal that its tune cycle has completed.
5. Unkey the radio's PTT.
6. Release the ALC injection (if it was asserted) and restore the radio's
   original mode (from the value captured in step 1).
7. Resume normal passthrough with no faking.

### Standalone operation

The ATU controller functionality must work with no PC connected at all —
this is a first-class use case, not just a side effect of the design:

- The tune button, the Icom ATU interface, the ALC injection circuit, and
  CAT control of the radio (Port 2) are all wired directly to the Arduino
  and don't depend on Port 1 having anything connected to it.
- The tune cycle captures its own pre-tune mode/PTT state directly from
  the radio (step 1 above) rather than depending on anything snooped from
  PC traffic — so it behaves identically whether a PC has ever been
  connected or not.
- With no PC, there's no CAT traffic on Port 1 to fake answers for or
  forward in the first place — the mode/PTT faking and live-forwarding
  behaviour above simply has nothing to do. It isn't a separate mode, it
  just naturally goes inert.

### PC transparency during a tune cycle

While a tune cycle is in progress, the PC must not be able to tell that
anything unusual is happening:

- Queries for mode or PTT/TX status are answered using the values captured
  at the start of the cycle (see above), not the radio's true current
  state.
- Every other CAT query or command is forwarded to/from the radio live, as
  normal — nothing needs to be cached or swallowed for these, since the
  tune cycle doesn't affect them.
- Commands from the PC that would directly conflict with the in-progress
  tune sequence (setting mode, or PTT, while the Arduino is mid-cycle) are
  silently swallowed — not forwarded to the radio — so the PC never sees
  an error and the tune cycle runs to completion undisturbed.
- Once the tune cycle completes and the radio is restored, the Arduino
  resumes transparent passthrough with no faking at all.

### Sequencer

A 4-band amplifier/relay sequencer, entirely independent of the CAT
broker and ATU logic above — it runs off the radio's STBY jack, not CAT,
so it works identically whether the PC or even the CAT link is present at
all.

- Each band (HF, 50, 144, 430MHz) has its own STBY input line (closure to
  ground = that band's TX requested) and its own independent 5-output
  sequence channel: `RX`, `SEQ1`, `SEQ2`, `SEQ3`, `TX`, with exactly one
  driven active at a time. All 5 drive a status LED; only `SEQ1`/`SEQ2`/
  `SEQ3` also drive a dedicated opto-isolator — `RX`/`TX` are indicator
  LEDs only, with no relay/opto function — giving 12 opto-isolator
  outputs total, 3 per band.
- A single **TX INHIBIT** output is shared across all 4 bands (the radio
  only ever transmits on one band at a time, so one inhibit line is
  enough) and asserted into the radio's TUNER-port TXINH input.
- **Up-sequence**, triggered the instant a band's STBY line asserts:
  1. Assert TX INHIBIT immediately, to hold off actual RF before the relay
     chain has finished settling.
  2. Step that band's channel through its outputs in order, de-energising
     the previous output as the next one energises: `RX → SEQ1 → SEQ2 →
     SEQ3 → TX`, with a hold between each step (exact timing TBD — see
     open questions).
  3. Once the sequence reaches `TX`, release TX INHIBIT so the radio can
     actually transmit.
- **Down-sequence**, triggered when that band's STBY line de-asserts:
  mirrors the up-sequence in reverse — `TX → SEQ3 → SEQ2 → SEQ1 → RX` —
  bringing relays back down safely once transmission has stopped, ending
  with `RX` energised again (rather than all outputs off), matching the
  fact that `RX` is itself a distinct, actively-driven output.
- STBY lines are watched via hardware interrupts (D2/D3/D18/D19 — see pin
  plan above) specifically because the lead time between STBY asserting
  and the radio actually transmitting may be very short; catching the
  edge immediately, rather than via a polling loop, is what makes it
  possible to assert TX INHIBIT in time.
- TX INHIBIT is wired to TUNER connector pin 8, alongside power (pin 1)
  and ground (pin 2) only. The FT-847 manual warns that CAT cannot be used
  while something is connected to the TUNER port — but that interlock is
  believed to be tripped specifically by the connector's separate Tuner
  Sense pin, which is deliberately left unconnected. See the reference
  section below for the full reasoning; still worth a bench check before
  relying on it in the field.

### Debug/control port

Serial0 (USB) is available for logging Arduino/CAT activity and for local
control of the sequencer, independent of the CAT passthrough path.

## Yaesu FT-847 CAT protocol (reference)

Sources: [Hamlib/Hamlib](https://github.com/Hamlib/Hamlib) —
[`rigs/yaesu/ft847.c`](https://github.com/Hamlib/Hamlib/blob/50b2a9310edc4a481d8a6ef10e948ea8554f97f9/rigs/yaesu/ft847.c) /
[`ft847.h`](https://github.com/Hamlib/Hamlib/blob/50b2a9310edc4a481d8a6ef10e948ea8554f97f9/rigs/yaesu/ft847.h) —
and `docs/ft-847_manual.pdf` ("Rear Panel Connectors", item 4).

- **Cabling**: the radio's CAT port (rear panel item 4, DB-9) expects a
  **null-modem** (crossed TX/RX) RS-232C cable, not a straight-through
  one — the manual is explicit that no external level converter should be
  needed from a PC's RS-232 port, i.e. the radio's own CAT port is already
  at proper RS-232 voltage levels. Port 2 (Arduino↔radio, via the
  MAX202CPE) should be wired the same crossed way.

- **Framing**: fixed 5-byte packets, `[P1][P2][P3][P4][OpCode]`, sent
  MSB-first. Both commands to the radio and its responses use this same
  5-byte shape (a couple of "get status" queries reply with a single byte
  instead — see below).
- **Serial settings**: **57600 baud** (the FT-847's CAT-rate menu supports
  4800–57600; 57600 is used here for headroom on satellite/Doppler-correction
  work). Must match on both the PC↔Arduino and Arduino↔radio links, and on
  the radio's own CAT-rate menu setting. 8 data bits, **2 stop bits**, no
  parity, no handshaking. Hamlib inserts a ~50ms delay before and after each
  write (`FT847_WRITE_DELAY` / `FT847_POST_WRITE_DELAY`) — sequential fast
  writes can otherwise confuse the radio.
- **Frequency encoding**: packed as 8-digit BCD across `P1..P4`
  (big-endian nibbles), in units of 10Hz, i.e. `BCD(freq_hz / 10)`.
- **Relevant opcodes** (`P1 P2 P3 P4 OpCode`, hex):
  | Command | Bytes |
  |---|---|
  | CAT on | `00 00 00 00 00` |
  | CAT off | `00 00 00 00 80` |
  | PTT on | `00 00 00 00 08` |
  | PTT off | `00 00 00 01 88` |
  | Set freq, Main VFO | `<BCD freq/10> 01` |
  | Set mode, Main VFO | `<mode> 00 00 00 07` |
  | Get RX status | `00 00 00 00 E7` → 1-byte reply |
  | Get TX status | `00 00 00 00 F7` → 1-byte reply |
  | Get freq+mode, Main | `00 00 00 00 03` → 5-byte reply (BCD freq + mode byte) |

  Mode byte values: `0x00` LSB, `0x01` USB, `0x02` CW, `0x03` CWR, `0x04`
  AM, `0x08` FM (narrow variants OR in `0x80`, e.g. `0x84` AMN).

- **Status byte decoding**:
  - RX status reply: bit 7 (`0x80`) clear = signal present (squelch open);
    bits 4:0 = S-meter reading.
  - TX status reply: bit 7 (`0x80`) clear = PTT on/transmitting, set = PTT
    off/receiving; bits 4:0 = power/ALC meter reading.

- **No RF power level command exists.** The FT-847's CAT protocol has no
  opcode to read or set output power — confirmed by Hamlib's own
  capabilities table (`.has_set_level = RIG_LEVEL_BAND_SELECT` only, no
  `RIG_LEVEL_RFPOWER`), and independently confirmed by `docs/YT847ManualRevA.pdf`
  (LDG's commercial YT-847 auto-tuner, built specifically for this radio).
  Its manual says a tune cycle will "change the power level to one that is
  appropriate for tuning", but this is not a discrete CAT power-set command
  — it's describing the side effect of switching the radio to **AM mode**,
  whose transmit power ceiling the FT-847 caps far lower than SSB/CW/FM
  (25W AM vs 100W SSB/CW on HF, per Hamlib's `tx_range_list`). **Resolution
  for this project: switching to AM mode before keying PTT is the power
  reduction** — there is no separate power-set step, and none is needed.
- **Early FT-847 units had genuinely unidirectional CAT — a separate issue
  from the SAT-mode trick below.** Confirmed directly in the Hamlib source
  comments: "The FT-847, as originally delivered, could not poll the radio
  for frequency and mode information. This was added beginning with the
  8G05 production runs" (serial numbers before roughly `8G05`, pre-May
  1998). Hamlib even ships a separate `FT847UNI` rig model for those units
  that tracks state purely in software instead of querying the radio,
  because querying wasn't possible at all. **Later units (like this
  project's radio) support direct polling** — `Get freq+mode, Main`
  (opcode `0x03` in the table above) works fine without any mode-switching
  trick.
- **Separately, the YT-847 still flips to Satellite mode to read the TX
  frequency, even on later/bidirectional units.** Per its manual: "Yaesu
  did not provide for a method to accomplish this in 'normal' mode, so the
  YT-847 switches the radio into Satellite mode to perform tuning." This
  isn't the same old-firmware bug — it's because Normal-mode polling only
  ever returns the single Main VFO's frequency, which isn't necessarily
  the true TX frequency under split operation. The YT-847 needs that
  certainty because it stores tuning parameters per exact TX frequency.
  **This project doesn't need the SAT-mode trick**: we're not doing
  split-aware, per-frequency tuning memory, so directly polling Main VFO
  (opcode `0x03`) whenever the current frequency is needed is enough.
- **Mic audio is live during the tune keying window.** Because the tune
  cycle works by switching to AM mode and keying PTT, whatever the
  operator says into the mic during that window is transmitted. The YT-847
  manual calls this out explicitly as something to be aware of; worth
  deciding whether this project should mute audio somehow, or just accept
  it as the AH-4/YT-847 both do.
- **The commercial precedent validates this project's broker design.** Per
  the manual: the YT-847's own PC-facing CAT pass-through "waits for idle
  CAT activity before controlling the transceiver" and only takes over the
  bus when it's idle, transparently to the host software — exactly the
  passthrough-with-takeover architecture this project is built around.

## Icom AH-4 ATU interface (reference)

Sources: `docs/Icom AH4 SGC Tuner Protocol Converter.pdf` (captured timing
diagrams for the genuine Icom AH-4 protocol) and
[doumae/fakeFC](https://github.com/doumae/fakeFC) —
[`fakeFC.ino`](https://github.com/doumae/fakeFC/blob/9eb30efeaafe0d38e46a250920fb501777e3a7f1/fakeFC.ino)
(an Arduino sketch that drives a real AH-4 from a Yaesu tuner port).

- **Physical interface**: a 4-wire connector — `START` (input to the
  tuner), `KEY` (output from the tuner), `+13.8V`, `GND`. Both `START` and
  `KEY` are **active-low** on the real AH-4 wiring (the PDF calls them
  `START_L` / `KEY_L`): a line is "asserted" by pulling it to ground, and
  idles high (pulled up).
- **Sequence** (from the PDF's "AH4 Normal Tuning Sequence" diagram, and
  matching fakeFC's state machine):
  1. Controller asserts `START` (pulls low) to begin a cycle.
  2. If `START` is released again within ~100ms, that's treated as a
     bypass/mode-toggle request, not a real tune (not something we need).
  3. If `START` stays asserted past ~100ms, the tuner responds by asserting
     `KEY` (pulls low) roughly 10ms later — this is the tuner's "busy,
     apply RF now" signal.
  4. The tuner remains busy for up to ~2–2.5 seconds (fakeFC times out the
     wait at 2500ms).
  5. When tuning finishes, the tuner releases `KEY` (goes high). This edge
     is the "tune cycle complete" signal.
  6. The controller then releases `START`.
- **Practical sequence for this project** (Arduino driving a real AH-4
  directly, not through the Yaesu tuner-port protocol fakeFC emulates):
  1. Pull `START` low and hold it (>150ms, comfortably past the 100ms
     bypass threshold).
  2. Once `START` has been asserted, key the radio's PTT via CAT so the
     AH-4 sees RF.
  3. Watch `KEY`: if it never asserts within ~500ms, there's no AH-4
     responding (error case). Once asserted, wait for it to release again
     (busy phase, ~2.5s timeout).
  4. On `KEY` release, re-sample ~25ms later to debounce/confirm (fakeFC
     does this — a bounce back to asserted within that window means the
     tune failed rather than succeeded).
  5. Unkey PTT via CAT, release `START`, then restore the radio's prior
     power/mode via CAT.
- fakeFC also emulates the separate Yaesu FC-40 tuner-port protocol (a
  different connector/protocol from CAT, using single-byte `0xa0`/`0xa1`
  status codes and `TXINH`/`TXGND` lines) so a Yaesu radio's native tuner
  port can drive a non-Yaesu tuner. That part isn't relevant to us — we are
  driving the AH-4's `START`/`KEY` lines directly from the Arduino and
  controlling the radio purely over CAT, not emulating a Yaesu tuner port.

## ALC injection for true minimum power (reference)

Carried over from a prior project; the FT-847-specific details below are
now confirmed against `docs/ft-847_manual.pdf` ("Rear Panel Connectors",
item 11).

- **Concept**: an Arduino PWM pin drives a charge pump producing roughly
  -4V. That voltage is gated onto the radio's ALC (Automatic Level
  Control) input line through an opto-isolator wired as a simple on/off
  switch (not used for linear/analog transfer — just to connect or
  disconnect the -4V rail from the ALC line).
- **Why**: transceivers normally use the ALC line to accept a feedback
  voltage from an external linear amplifier, telling the radio to back off
  drive if the amp reports it's being overdriven. Injecting an artificial
  ALC voltage forces the radio's own gain-reduction loop to turn down RF
  output — a genuine hardware-level power reduction, independent of any
  CAT command, and capable of going lower than AM mode's power ceiling
  alone.
- **Relationship to AM mode**: the two are complementary, not
  alternatives. AM mode is switched to regardless (see the CAT reference
  above — the FT-847 offers no other way to run a controlled tune cycle
  over CAT), and the ALC injection is layered on top of that to pull
  power down further during the keyed window.
- **Confirmed FT-847 connector and spec**: rear panel item **(11) EXT
  ALC**, an RCA female jack. Per the manual: "The specified control
  voltage range is 0V ~ −4V DC, with −4V corresponding to the maximum
  degree of power reduction applied to the transceiver." This matches the
  charge pump's ~-4V output and the assumed polarity exactly (0V = full
  power, -4V = maximum reduction) — no further voltage/polarity
  verification needed. Signal is presumably on the RCA center pin, ground
  on the shield (standard RCA convention; worth a continuity check against
  the actual radio before connecting).
- Also present on the rear panel: **(10) EXT PTT**, a separate RCA female
  jack wired in parallel with the front-panel MOX switch — open-circuit
  (+5V) = receive, closed to ground (1mA) = transmit. Not needed for this
  project since PTT is already handled over CAT, but it's available as a
  hardware-level PTT alternative if CAT keying ever proves unreliable. The
  manual notes this jack is for external PTT *input* only — it should not
  be used to switch amplifiers or other external devices (that's what the
  separate STBY jack is for).

## STBY jack, TUNER port, and TX INHIBIT (reference)

Sources: `docs/ft-847_manual.pdf` ("Linear Amplifier Interfacing" and
"Rear Panel Connectors" sections) and pinouts confirmed directly by
inspection/prior documentation of the connectors themselves.

- **STBY connector (rear panel item 8, 5-pin mini-DIN)** — confirmed
  pinout:

  | Pin | Signal | Arduino pin |
  |---|---|---|
  | 1 | GND (common) | — |
  | 2 | 430MHz | D3 |
  | 3 | HF | D2 |
  | 4 | 144MHz | D18 |
  | 5 | 50MHz | D19 |

  Per the manual, these are "closure-to-ground" open-collector T/R lines,
  one per band, rated +24V DC / 100 mA max, **positive DC only** — "not
  compatible with negative DC voltages, nor AC voltages of any
  magnitude." All 4 lines land on the Mega's interrupt-capable pins (see
  Pin plan) so a TX request can be caught immediately, which matters given
  how little lead time there may be before the radio actually transmits.
- **TUNER connector (rear panel item 5, 8-pin mini-DIN)** — confirmed
  pinout (partial, relevant pins only):

  | Pin | Signal | Arduino pin |
  |---|---|---|
  | 1 | +13.8V | — |
  | 2 | GND | — |
  | 8 | TX INHIBIT | D4 |

  There is also a separate **Tuner Sense** pin on this connector, believed
  to be what actually triggers the FT-847's CAT-disable interlock (see
  below) — **deliberately left unconnected**. Only pins 1, 2, and 8 are
  wired in.
- **⚠ Possible pin-2 discrepancy, worth double-checking.** fakeFC's own
  schematic (`docs/fakeFC/docs/fakeFC-circuit_prototype01.png` —
  `fakeFC-prototype01.kicad_sch`) labels the equivalent 8-pin connector as
  `1=13.8V, 2=TX_GND, 3=GND, 4=DATA_IN, 5=DATA_OUT, 6=SENSE, 7=RESET
  (unconnected), 8=TX_INH`. Their pin 2 (`TX_GND`) is wired through a 1kΩ
  resistor to a separate Arduino pin configured `INPUT_PULLUP` — and in
  firmware it's *only ever read* (`digitalRead`), never written, and its
  (inverted) value is continuously mirrored onto its own dedicated status
  LED every loop iteration. That combination — pulled up, polled, and
  worth a live indicator — only makes sense for a real, changing status
  signal, almost certainly the radio pulling it to ground while
  transmitting (the same closure-to-ground convention as the STBY lines),
  not a static ground reference; their real ground is the separate pin 3.
  That doesn't match "Pin 2 = Gnd" as documented for this project.
  **Decision: wire it as a signal, not ground** — pin 2 is connected to
  Arduino D5 as an input (mirroring fakeFC's own pin choice), with its
  actual use deferred for now. It is *not* tied to the project's ground
  rail, precisely because it may not be one.
- **TX INHIBIT electrical characteristics, per fakeFC's schematic**: their
  `TX_INH` (J1 pin 8) connects directly to a Sparkfun Pro Micro GPIO
  (running at 5V, confirmed by the schematic's onboard 7805 regulator)
  through nothing more than a 1kΩ resistor and an indicator LED — no
  transistor, opto-isolator, or level-shifter. This strongly suggests
  TX INHIBIT is a plain 0–5V TTL/CMOS-level digital input on the radio's
  side (unlike ALC's special -4V to 0V range), and that D4 can likely
  drive it directly.
  - **Polarity (inferred from firmware behaviour)**: `PIN_TXINH` is set
    `OUTPUT` and driven LOW at boot/idle, and only pulsed HIGH briefly
    (25–60ms) during specific protocol handshake states. This is
    consistent with **active-high**: HIGH = inhibit asserted, LOW
    (default/idle) = not inhibited — the opposite sense from the STBY
    lines' closure-to-ground (active-low) convention, so worth being
    careful not to mix the two up.
  - **Caveat**: fakeFC only ever asserts `TX_INH` while it's also actively
    exchanging bytes on `DATA_IN`/`DATA_OUT` (the real Yaesu tuner
    protocol) — never as a bare standalone signal on its own. Since this
    project's plan is to leave `DATA_IN`/`DATA_OUT` unconnected and assert
    TX INHIBIT bare, it isn't confirmed from this reference whether the
    radio treats it as a fully independent inhibit line, or expects it
    alongside an ongoing tuner-protocol conversation. Worth including in
    the bench test alongside the CAT-interlock check.
  - Also worth noting: the schematic's `SENSE` pin (6) matches the "Tuner
    Sense" terminology used for this project, which is reassuring, but its
    exact wiring/destination in fakeFC's circuit wasn't legible enough at
    the available resolution to independently confirm the CAT-disable
    hypothesis from the schematic alone.
- **The CAT/TUNER-port conflict, and how it's avoided.** The manual states,
  in the CAT programming section: "**Important Notice!** It is not
  possible to engage the CAT System when the FC-20 Automatic Antenna Tuner
  is in use. Please disconnect the FC-20 Control Cable from the TUNER jack
  on the rear panel of the FT-847 prior to commencing CAT System control
  of the FT-847." This looked like a serious risk to this project's
  architecture, since TX INHIBIT lives on that same connector (matching
  fakeFC's `PIN_TXINH`) — but the interlock is believed to be driven
  specifically by the separate Tuner Sense pin, not by TXINH/power/ground.
  By leaving Tuner Sense unconnected and wiring only power, ground, and
  TX INHIBIT, the radio should never detect a tuner as "present" and CAT
  should keep working normally. **This is a belief, not yet bench-verified
  against this specific radio** — worth confirming with a simple test
  (wire it up, confirm CAT still responds on Port 2) before relying on it.
- **The YT-847 precedent** supports this being a reasonable approach: its
  manual's install steps connect its interface cable to *both* the TUNER
  jack and the CAT jack simultaneously, and CAT still works for it — its
  own text says the TUNER jack is used *only* to draw +12V power from the
  radio, with all actual radio control happening over the separate CAT
  port, never speaking the FC-20 tuner protocol. Same underlying strategy:
  don't engage whatever it is on the TUNER connector that trips the
  interlock.

## Open questions / to be determined

- Whether the AH-4's active-low, open-collector-style `START`/`KEY` lines
  need transistor buffering/level shifting on our side (the SGC converter
  design uses MOSFETs for this; our AH-4-only case may be simpler since
  we're not also bridging to a second tuner's protocol).
- Debounce/timing behaviour of the physical tune button (D7), and exact
  blink/status patterns for the tune LED (D8).
- **What to do with TUNER pin 2 / D5** (wired in as `TX_GND` per fakeFC's
  schematic, purpose deferred): confirm on the bench whether it's really
  an active closure-to-ground status signal (and if so, what it
  indicates — TX status, tuner-presence, something else) or turns out to
  just be ground after all, distinct from the connector's pin 3.
- **Confirm TX INHIBIT polarity/voltage** (TUNER connector pin 8):
  fakeFC's firmware behaviour suggests active-high, plain 0–5V logic,
  drivable directly from D4 with no isolation — but this is inferred from
  a different device's firmware, not measured on this radio directly, so
  worth confirming on the bench.
- **Bench-verify that leaving Tuner Sense unconnected really does avoid
  the CAT-disable interlock**, and that TX INHIBIT works as a standalone
  signal without the DATA_IN/DATA_OUT tuner-protocol traffic fakeFC always
  pairs it with — wire up only power/ground/TX INHIBIT (pins 1, 2, 8) and
  confirm both that CAT still responds correctly on Port 2, and that
  asserting TX INHIBIT alone actually holds off transmission.
- The 12 sequencer opto-isolator output pins (`SEQ1`/`SEQ2`/`SEQ3` × 4
  bands) — to be specified.
- Per-step sequencer timing: how long each of `SEQ1`/`SEQ2`/`SEQ3` is held
  before advancing to the next step (and the reverse, on the way down) —
  likely needs to be tuned against the actual amplifier relays' switching
  times, so may end up configurable rather than fixed.
