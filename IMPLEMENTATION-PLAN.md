# Implementation Plan

Phased build order for the firmware described in [README.md](README.md). The
README stays the source of truth for *behaviour*; this file only covers *order
of work* and *how each phase is tested*.

## Guiding principles

- **Each phase ends with something demonstrable on the bench**, and no phase
  needs RF until Phase 8. Dummy load whenever the radio is keyed.
- **Non-blocking everywhere.** No `delay()` in the main path: the sequencer,
  CAT passthrough, and tune cycle all run concurrently (the sequencer must keep
  working while CAT bytes flow; STBY may fire mid-tune). Use `millis()`-based
  state machines.
- **Keep logic pure, keep hardware at the edges.** Frame parser, sequencer state
  machine, band lookup, config validation and the tune state machine take time
  and I/O through small interfaces, so they can run in `pio test -e native` on
  the PC with a fake clock. Only thin wrappers touch `digitalWrite`/`Serial`.
- **Fail safe.** Any abort path (timeout, no KEY, CAT error, watchdog) must end
  with PTT off, START released, ALC released, TX INHIBIT in a known state, and
  the radio's mode restored where possible.
- **Suggested module layout** (all under `src/`): `pins.h` (exists), `log`,
  `cat_frame` (parser/encoder), `cat_bridge` (Port1↔Port2 + intercepts),
  `radio` (own CAT queries/commands on Port 2), `sequencer`, `config`
  (JSON+EEPROM), `ah4`, `alc`, `tune` (orchestrator), `main.cpp` (wiring only).

## Phase 0 — Toolchain and hardware smoke test

Scaffold builds and flashes (done). Remaining:

- Add `ArduinoJson` to `lib_deps`, add a `native` env for unit tests, pin
  `upload_port`/`monitor_port` (or document `--upload-port`; ttyUSB0 also exists).
- Serial0 at 115200 with a boot banner and a simple heartbeat on the activity LED.
- Walk-test: a temporary debug command that pulses every output pin in turn
  (LEDs, opto drivers, ALC gate, START, TX INHIBIT) so the wiring against
  [src/pins.h](src/pins.h) and the schematic can be checked with a meter/LEDs.
  Also echo the state of D5, D6, D11 and the four STBY inputs.

**Done when:** every output pin verified against the schematic; every input
reads correctly when jumpered to ground.

## Phase 1 — Sequencer core (no radio, no config)

Hardcoded default timings and tune profiles (compiled in). Non-blocking state
machine per band: up-sequence, down-sequence, TX INHIBIT handling, RX/TX LED
rules (cumulative `SEQ1..3`), re-entrancy/idempotency, and the per-band
"tune-profile hold" flag that suppresses STBY handling.

- STBY inputs on interrupts (D2/D3/D18/D19): the ISR only records the edge/level;
  the state machine acts in the main loop, unless bench measurement shows the
  TX INHIBIT assertion must happen in the ISR itself (see README: lead time).
  Decide by measurement, keep the ISR minimal either way. Include debounce.
- Band mapping: STBY pin ↔ band index ↔ output group (note the different orders
  in the README).
- Handle interrupted sequences correctly (STBY drops mid-up-sequence, then
  re-asserts mid-down-sequence).
- Serial0 log lines with timestamps for every transition.

**Test:** native unit tests with a fake clock (full up/down timing, aborted
sequences, tune-profile skipping stages, hold flag suppressing STBY). Bench:
jumper STBY pins to ground by hand and watch LEDs / scope the opto outputs.

**Done when:** all 4 bands sequence correctly and independently on the bench
with the radio disconnected.

## Phase 2 — Config: JSON, EEPROM, cross-band triggers

- Schema per [config/sequencer.json](config/sequencer.json): validation,
  `schema_version` check, band edges, timing, tune profile, triggers.
- `CONFIG` / `READY` / `OK` / `ERROR: <reason>` handshake on Serial0, with a
  timeout so a half-sent config can't wedge the port.
- EEPROM persistence with magic/version/CRC; fall back to compiled defaults if
  empty or invalid. Resolve the open question of how defaults are produced
  (recommend a hand-written fallback table kept in sync by a native test that
  compares it with `config/sequencer.json`).
- Cross-band triggers: source `SEQ1` on/off snaps the target output.
  Define what happens when two rules target the same output, or the target band
  is itself active.
- Test [tools/send_config.py](tools/send_config.py) end to end.

**Test:** native tests for validation (good file, missing fields, bad types,
overlapping/inverted band edges, oversized), plus bench round-trip and a power
cycle to prove persistence and fallback (corrupt the EEPROM on purpose).

**Done when:** a config change takes effect and survives power-off; bad configs
never leave the sequencer un-runnable.

## Phase 3 — CAT frame layer and transparent passthrough

Start with **no radio**: two USB-serial adapters (or a PC loopback) on Port 1 /
Port 2, plus the MAX202 wiring checked.

- Frame model: 5-byte commands, 5-byte / 1-byte replies. The key design point is
  that the reply length depends on the *command opcode*, so the bridge tracks the
  outstanding request per direction to frame replies correctly (`0xE7`/`0xF7`
  → 1 byte, `0x03` → 5 bytes, others → none/echo as the radio does).
- Byte-transparent passthrough Serial2 ↔ Serial3, 57600 8N2, with buffer sizing
  for the Mega's 64-byte default RX buffers (raise if needed) and no dropped bytes.
- Inter-byte / frame timeouts so a torn frame can't desync the parser forever.
- Log (optionally, off by default) each decoded frame on Serial0.

**Test:** native tests for the parser (split frames, garbage, resync). Bench: run
flrig/hamlib against the Arduino with a real FT-847 (or a small radio
simulator script on the Port 2 side) and confirm behaviour is identical to a
direct connection, and throughput/latency is unchanged.

**Done when:** flrig works through the bridge against the real radio for an
extended session with zero errors, and the sequencer (Phase 1) still meets its
timing while CAT traffic is flowing.

## Phase 4 — Arduino-originated CAT (`radio` module) and bus arbitration

- Own queries/commands on Port 2: CAT on, get freq+mode (`0x03`), get TX status
  (`0xF7`), set mode (`0x07`), PTT on/off (`0x08` / `0x88`), with the ~50ms
  pre/post write spacing the README notes.
- **Arbitration:** the Arduino only injects a command when the bus is idle (no
  PC request outstanding, per the YT-847 precedent), and holds off PC traffic
  while its own exchange is in flight, buffering or delaying rather than
  dropping. Works identically with no PC attached.
- BCD frequency encode/decode; mode byte handling (incl. narrow `0x80` variants
  so restore is exact); map frequency → band via config.
- Debug commands on Serial0 to trigger each of these by hand.

**Test:** native tests for BCD/mode helpers and arbitration; bench with the real
radio with PC polling running concurrently, verifying no corrupted or lost
replies on either side.

**Done when:** the Arduino can read frequency/mode/TX status and set mode
reliably while a PC polls at full rate, and standalone with no PC.

## Phase 5 — Radio-side bench checks (TX INHIBIT / STBY / CAT interplay)

These answer the README's open questions before the tune cycle depends on them.
Dummy load; low power.

- With the TUNER connector wired as designed (pins 1, 2, 8; Sense unconnected):
  confirm CAT still responds while TX INHIBIT is held high, that no RF is
  produced while inhibited, and that STBY still asserts.
- Measure STBY-assert → RF latency for a manual PTT and for a CAT PTT (`0x08`).
  Records whether CAT PTT asserts STBY at all, and how much lead time the
  sequencer really has. Feeds the ISR-vs-loop decision from Phase 1.
- Sanity-check D5 (`TX_GND`) behaviour on HF/50MHz vs the STBY lines.

**Done when:** each open bench question in the README is answered and the
README is updated with the results.

## Phase 6 — AH-4 interface (`ah4` module) and ALC injection

Can proceed in parallel with Phases 3–5 if hardware is ready; they're
independent of CAT.

- `START`/`KEY` driver state machine: hold START (>150ms), wait for KEY assert
  (≤~500ms, else "no ATU"), wait for KEY release (≤~2.5s timeout), re-sample
  after ~25ms, report success/fail/timeout. Non-blocking, with an explicit
  `abort()` that always releases START.
- ALC: charge-pump PWM level, opto gate, settle time; verify -4V output
  unloaded and gated. Make the PWM level a compile-time or config constant so it
  can be tuned.
- Confirm optos against the real AH-4 (pull-up question from the README).

**Test:** a simulated AH-4 (second Arduino or a switch/pot-driven jig pulling KEY
low with timing) to exercise success, no-response, timeout, and bounce cases.
Meter the ALC output. Then connect the real AH-4 with no RF (START/KEY
handshake only).

**Done when:** the driver reports the right outcome for all simulated cases and
the real AH-4 responds to START with KEY as expected.

## Phase 7 — Tune cycle orchestrator, no faking yet

The 11-step sequence from the README, as a single state machine with a clear
abort path. Tune button (D6, debounced, polled), tune LED patterns (D7),
activity LED meaning (D8) — decide and document.

Order matters and is called out in the README: sequencer tune-profile up-sequence
*before* START, hold flag set for the band, then AM + ALC, START, PTT, wait for
KEY, PTT off, down-sequence, release hold, ALC off, restore mode.

- Every step has a timeout and a defined abort/cleanup path; test the cleanup
  from *each* step.
- Refuse to start when already transmitting, when the band can't be
  determined, or when a config/CAT query fails.
- Restore-mode failure handling (retry, then log loudly).

**Test:** native tests of the state machine with mocked radio/ATU/sequencer,
injecting a failure at every step. Bench, stage by stage with a dummy load and
low power: (a) sequencer + CAT only, START disabled; (b) AH-4 simulator; (c) real
AH-4 into a dummy load; (d) real AH-4 on the antenna.

**Done when:** a button press completes a tune cycle standalone (no PC) and every
injected failure returns the radio to a clean idle state.

## Phase 8 — PC transparency during a tune cycle

- Splice the *faked* pre-tune mode byte into the live `0x03` reply (real
  frequency, captured mode); report PTT off in `0xF7` replies; forward everything
  else live.
- Silently swallow PC mode/PTT commands during the cycle, and make sure no reply
  the PC is waiting for is left dangling (decide per opcode whether to synthesise
  an ack, based on what the radio actually returns — check with real captures).
- Handle a PC request that is in flight at the moment the cycle starts or ends.

**Test:** native tests using recorded frames; bench with flrig polling during a
tune, and a Serial capture on Port 1 compared with a no-tune baseline to prove
the PC sees no difference.

**Done when:** flrig shows no mode change or PTT blip, and reports no errors,
throughout a tune cycle.

## Phase 9 — Hardening and release

- Watchdog enabled, with reset-cause logged at boot; make sure a reset mid-tune
  leaves outputs safe (defaults on boot: PTT unkeyed by definition, START
  released, ALC gate off, TX INHIBIT not held, sequencer idle).
- Soak test: hours of PC polling, repeated tune cycles, random STBY activity.
- Stress the config handshake (garbage on Serial0, partial JSON, unplugging).
- Decide the mic-audio-during-tune question from the README.
- Finalise README (results of the bench questions), and add a short
  build/flash/config section to it.

## Cross-cutting notes

- **Risk to retire early:** the TX INHIBIT / TUNER-port CAT interlock
  (Phase 5) and STBY lead time. If the assumptions fail, the sequencer design
  changes, so don't build the tune orchestrator on top of unverified beliefs.
- **Dependencies:** 1 → 2; 3 → 4 → 7; 5 needs 1 and 4; 6 is independent until 7;
  8 needs 3 and 7. A sensible solo order is 0, 1, 3, 4, 2, 5, 6, 7, 8, 9, and
  Phase 2 can slip later since Phase 1 runs on compiled defaults.
- **Commit rhythm:** one commit (or PR) per phase, with its native tests, and
  the README updated in the same commit whenever a bench result settles an open
  question.
