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
| D2, D3, D18, D19 | **reserved, unused** — for the future STBY connection (4 band-specific T/R lines: HF/50/144/430MHz) |
| D20, D21 (I2C: SDA/SCL) | **kept free** — not used by anything, available for a future I2C peripheral (e.g. a status display) |

Only six Mega 2560 pins support true external interrupts: D2, D3, D18,
D19, D20, D21. Putting both CAT links on Serial2/Serial3 (rather than
Serial1) means Serial1's pins — D18/D19, which are two of those six
interrupt-capable pins — are never touched, so **all six** interrupt pins
stay free instead of just four. Four of them (D2, D3, D18, D19) are
earmarked for the eventual STBY connection; the remaining two (D20/D21)
are then free of any reservation and can be used for I2C later without
conflicting with STBY. The tune button (D7) doesn't need an
interrupt-capable pin — it's polled in the main loop — so it and the
status LED (D8) sit comfortably outside the reserved set.

## Behaviour

### Normal operation (passthrough)

- Bytes arriving on Port 1 (from the PC) are forwarded to Port 2 (to the
  radio), and bytes arriving on Port 2 (from the radio) are forwarded to
  Port 1 (to the PC).
- While relaying, the Arduino snoops the traffic and maintains a cache of the
  radio's last-known state relevant to tuning: frequency, mode, RF power
  level, and PTT/TX status (and any other CAT parameters needed later).

### Tune cycle (triggered by the tune button)

1. Issue the appropriate start sequence to the Icom ATU interface.
2. Take over the CAT link to the radio (without forwarding PC traffic
   verbatim during this window) and:
   - Select AM mode (there is no CAT power-set command on this radio —
     switching to AM mode is itself the main power reduction, since the
     FT-847 caps AM output much lower than SSB/CW/FM; see the CAT
     reference below).
   - Optionally assert the ALC injection circuit to pull power down
     further, closer to true minimum, independent of CAT.
   - Key the radio's PTT.
3. Wait for the ATU to signal that its tune cycle has completed.
4. Unkey the radio's PTT.
5. Release the ALC injection (if it was asserted) and restore the radio's
   original mode (from the cached pre-tune state).
6. Resume normal passthrough.

### PC transparency during a tune cycle

While a tune cycle is in progress, the PC must not be able to tell that
anything unusual is happening:

- Any CAT queries from the PC (e.g. frequency, mode, power, status) are
  answered by the Arduino directly, using the last cached values from before
  the tune cycle began, rather than forwarding them to the radio.
- Commands from the PC that would conflict with the in-progress tune sequence
  (e.g. changing power, mode, or PTT) are silently swallowed — not forwarded
  to the radio and not queued — so the PC never sees an error, and the tune
  cycle runs to completion undisturbed.
- Once the tune cycle completes and the radio is restored, the Arduino
  resumes transparent passthrough and the cache is refreshed from real radio
  traffic.

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
  split-aware, per-frequency tuning memory, so polling Main VFO directly
  (or just snooping PC↔radio traffic, our default approach) is enough.
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

## Open questions / to be determined

- Whether the AH-4's active-low, open-collector-style `START`/`KEY` lines
  need transistor buffering/level shifting on our side (the SGC converter
  design uses MOSFETs for this; our AH-4-only case may be simpler since
  we're not also bridging to a second tuner's protocol).
- Debounce/timing behaviour of the physical tune button (D7), and exact
  blink/status patterns for the tune LED (D8).
- Eventual STBY port wiring (rear panel item 8, 5-pin mini-DIN, one
  closure-to-ground T/R line per band: HF/50/144/430MHz) — pins are
  reserved (D2, D3, D18, D19) but the actual connection is future work.
