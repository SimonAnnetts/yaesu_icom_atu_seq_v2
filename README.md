# Yaesu/Icom ATU Sequencer v2

An Arduino Mega 2560 controller that lets an Icom AH-4-type automatic antenna
tuner work with a Yaesu FT-847, runs a four-band amplifier/preamp sequencer
alongside it, and does both without getting in the way of a PC that is
controlling the radio over CAT.

> `docs/Hamlib` and `docs/fakeFC` are local clones kept only for reference
> while researching the FT-847 CAT protocol and Icom AH-4 timing (see the
> reference sections below) — they're excluded via `.gitignore` and are not
> part of this repo. Their upstream sources are
> [Hamlib/Hamlib](https://github.com/Hamlib/Hamlib) and
> [doumae/fakeFC](https://github.com/doumae/fakeFC).

## Aims

1. **Use an Icom AH-4-type ATU with the Yaesu FT-847** (or any tuner that
   speaks the same START/KEY protocol, and possibly other Yaesu radios of the
   same era). The FT-847 has no way to drive such a tuner on its own. At the
   press of a tune button the Arduino runs the whole cycle: it sets the radio
   up over CAT (tune mode, keying PTT at the right moment), drives the
   tuner's START/KEY lines, watches for it to finish, and puts the radio back
   as it found it. Proven on an Alinco EDX-2, an Icom-compatible tuner.
   **This does not re-purpose the radio's own [TUNER] key** or any of its
   built-in tuner control: the device has **its own tune button** (and tune
   indicator), and the radio's rear TUNER connector is used only for the TX
   INHIBIT line and its supply and ground. Its tuner-sense pin is deliberately
   left unconnected, so the radio never believes a tuner is attached and CAT
   keeps working.
2. **Provide three sequencer outputs per band, for four bands** (HF, 50, 144
   and 430MHz): `SEQ1`–`SEQ3` opto-isolated outputs for preamp and power-amp
   relays, plus `RX`/`TX` indicator LEDs, driven from the radio's STBY jack,
   with a shared **TX INHIBIT** line that holds off the transmitter until
   each band's sequence has finished settling.
3. **Provide an ALC output that can control the radio's power during the tune
   cycle.** A charge pump makes about -4V, gated by an opto-isolator onto the
   radio's EXT ALC jack, so the carrier can be trimmed to what the tuner
   wants (an AH-4 needs about 10W). The circuit is built and the voltage
   reaches the jack; the radio is not yet responding to it, so it is optional
   and off by default.
4. **Allow complex inter-band sequencer and tuner configurations to be defined
   in a JSON config**: per-band delays, tune profiles, band edges, which bands
   the tuner may be used on, and cross-band rules (for example, transmitting
   on 50MHz switches off a 144MHz masthead preamp). The config is uploaded
   over USB, validated, and kept in EEPROM; a bad one can never leave the
   sequencer unable to run.
5. **Still let a PC control the radio over CAT, with none of the above getting
   in the way.** The Arduino sits between the PC and the radio and passes CAT
   traffic through byte for byte. While a tune cycle needs the radio, the PC
   keeps being answered (from a snapshot of the radio, with the original mode
   and no PTT), so it cannot tell a tune is happening.

**The PC is optional.** With no PC connected at all, the tune button, the
tuner and the sequencer all work exactly the same way. Standalone operation is
a first-class requirement, not a side effect.

## How it fits together

```
 PC (flrig etc.) ── Port 1 ──┐                        ┌── CAT ── Port 2 ── FT-847
                             │                        │
                       Arduino Mega 2560 ─────────────┼── STBY jack (4 bands in)
                             │                        ├── TUNER connector: TX INHIBIT out
   tune button ──────────────┤                        └── EXT ALC jack  ←── charge pump + gate
   tune buzzer ──────────────┤
   AH-4-type ATU ─ START/KEY ┤        12 opto outputs (SEQ1-SEQ3 × 4 bands) + RX/TX LEDs
                             └── USB (Serial0): logging, bench keys, JSON config upload
```

The CAT proxy is the *means*, not the goal: it is what lets aims 1 and 5 hold
at the same time (the tune cycle needs the radio's CAT port while the PC is
still using it). The sequencer (aim 2) does not use CAT at all — it runs from
the STBY lines, so it works with the CAT link absent.

Where to look in this document: **Hardware** and the **Pin plan** for the
wiring; **Behaviour** for the tune cycle, the sequencer and the config; the
**reference** sections for the FT-847 CAT protocol, the AH-4 interface and the
ALC circuit; **Bench results** for what has been verified on the real radio
and tuner; **Open questions** for what has not.

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
- **Tune button**: the device's own physical pushbutton, wired to an Arduino
  input, used to initiate a tune cycle. It is separate from the radio's
  front-panel [TUNER] key, which is not used or intercepted.
- **ALC injection circuit**: a charge pump driven from an Arduino PWM pin
  (producing roughly -4V), gated onto the radio's ALC line through an
  opto-isolator used purely as a switch. This lets the Arduino trim the
  radio's RF output to the ~10W carrier the AH-4 wants during a tune cycle
  (it aborts outside 5–15W — see the AH-4 reference below), independent
  of — and in addition to — the AM-mode power ceiling (see below). Carried over from a prior project; confirmed against the FT-847
  manual — see the ALC reference section further down.
- **Amplifier sequencer**: 4 independent relay sequencers, one per band —
  Band 1=HF, Band 2=50MHz, Band 3=144MHz, Band 4=430MHz — each with 5
  distinct outputs —
  `RX`, `SEQ1`, `SEQ2`, `SEQ3`, `TX`. These are cumulative, not mutually
  exclusive: `SEQ1`-`SEQ3` latch on in order and stay on together for the
  whole transmission, while `RX`/`TX` are boundary-condition indicator
  LEDs (see "Sequencer" under Behaviour for the exact timing). All 5 drive
  a status LED; only `SEQ1`/`SEQ2`/`SEQ3` also drive an opto-isolator
  (`RX`/`TX` are LED indicators only, no relay/opto function) — 12
  opto-isolator outputs total, 3 per band (see Pin plan). Triggered by the
  radio's STBY jack (4 closure-to-ground T/R lines, wired to the
  interrupt-capable pins — see Pin plan). A single, shared **TX INHIBIT**
  output (D4) holds off the radio's actual transmit output while each
  band's sequence runs, wired to pin 8 of the radio's TUNER connector.
  Also configurable (JSON-based — see below) so that transmitting on one
  band can partially trigger another band's sequencer, e.g. disabling a
  masthead preamp on an unrelated band while transmitting nearby. See
  "Sequencer" under Behaviour, and the TUNER-port reference
  section further down for the confirmed pinout and how the CAT-disable
  risk is avoided (by deliberately leaving the TUNER connector's separate
  Tuner Sense pin unconnected).

### Pin plan (Mega 2560)

| Pin(s) | Function |
|---|---|
| D0/D1 (Serial0) | USB debug/control link to PC |
| D16/D17 (Serial2) | Port 1 — CAT to PC, via MAX202CPE |
| D14/D15 (Serial3) | Port 2 — CAT to radio, via MAX202CPE |
| D6 | Tune button (input, polled) |
| D7 | Tune indicator: passive buzzer, ~2048Hz PWM from Timer4 (OC4B) |
| D8 | Generic activity LED (output) |
| D9 | ALC injection charge pump (PWM output) |
| D10 | ALC opto-isolator gate (on/off switch) |
| D11 | Icom AH-4 `KEY` input, via opto-isolator, `INPUT_PULLUP` |
| D12 | Icom AH-4 `START` output, via opto-isolator |
| D2 | STBY: HF (input, interrupt) |
| D3 | STBY: 430MHz (input, interrupt) |
| D18 | STBY: 144MHz (input, interrupt) |
| D19 | STBY: 50MHz (input, interrupt) |
| D4 | Shared TX INHIBIT output → TUNER connector pin 8 |
| D5 | TUNER connector pin 2 (`TX_GND`) — input, `INPUT_PULLUP`. Open-collector, active-low: asserted (low) when the radio transmits on HF or 50MHz. Logged/cross-check only, not load-bearing (STBY already covers this with more granularity) |
| D22 / D23 / D24 / D25 / D26 | Sequencer Band 1 (**HF**): `RX` (LED only) / `SEQ1` (LED+opto) / `SEQ2` (LED+opto) / `SEQ3` (LED+opto) / `TX` (LED only) |
| D27 / D28 / D29 / D30 / D31 | Sequencer Band 2 (**50MHz**): `RX` / `SEQ1` / `SEQ2` / `SEQ3` / `TX` — same LED-only/LED+opto pattern as Band 1 |
| D44 / D45 / D46 / D47 / D48 | Sequencer Band 3 (**144MHz**): `RX` / `SEQ1` / `SEQ2` / `SEQ3` / `TX` — same pattern |
| D49 / D50 / D51 / D52 / D53 | Sequencer Band 4 (**430MHz**): `RX` / `SEQ1` / `SEQ2` / `SEQ3` / `TX` — same pattern; D50-D53 are the Mega's hardware SPI pins (MISO/MOSI/SCK/SS), repurposed as plain digital outputs since SPI isn't needed elsewhere in this project |
| D20, D21 (I2C: SDA/SCL) | **kept free** — not used by anything, available for a future I2C peripheral (e.g. a status display) |

Sequencer band↔channel mapping (Band 1=HF, Band 2=50MHz, Band 3=144MHz,
Band 4=430MHz) is independent of, and doesn't follow the same order as,
the STBY input pin assignment above (D2=HF, D3=430MHz, D18=144MHz,
D19=50MHz) — each STBY input simply needs to be cross-wired in firmware
to drive its matching band's sequencer channel, e.g. STBY D2 (HF) drives
Band 1 (D22-26), STBY D3 (430MHz) drives Band 4 (D49-53), and so on.

Only six Mega 2560 pins support true external interrupts: D2, D3, D18,
D19, D20, D21. Putting both CAT links on Serial2/Serial3 (rather than
Serial1) means Serial1's pins — D18/D19, which are two of those six
interrupt-capable pins — are never touched, so **all six** interrupt pins
stay free instead of just four. Four of them (D2, D3, D18, D19) are now
used for the 4 STBY band lines (interrupt-driven, since a TX request
needs to be caught immediately to start the sequencer); the remaining two
(D20/D21) stay free of any reservation for I2C. The tune button (D6)
doesn't need an interrupt-capable pin — it's polled in the main loop — so
it and the two status LEDs (D7, D8) sit comfortably outside the reserved
set. The 20 sequencer outputs (D22-D53) and the shared TX INHIBIT output
(D4) are plain digital outputs with no special pin requirements. The
Arduino-side band↔pin mapping for STBY (which of D2/D3/D18/D19
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

The tune cycle must also drive the amplifier sequencer for whichever band
it's about to transmit on — a tune cycle keys PTT just like any other
transmission, and the sequencer's relay chain has to be up before RF
appears, exactly as it would for a normal transmission.

A CAT-commanded PTT does assert the radio's STBY line(s) (bench-verified —
see "Bench results" below), but the lead time before RF is still unmeasured.
Rather than depend on that, the tune cycle **explicitly drives the
sequencer itself**, calling the same up/down-sequence logic the STBY
interrupt handler uses, directly and proactively:

1. Actively query the radio's current mode and frequency over CAT
   (`Get freq+mode, Main`, opcode `0x03`) and note the current PTT/TX
   status is "off" (this is always known without needing to ask, since
   the Arduino is the one about to turn it on). This captures the values
   that will need to be faked and restored, freshly, at the start of
   every cycle — so it works identically whether or not a PC (and any
   snooped history) exists.
2. Map that frequency to one of the 4 sequencer bands (see "Sequencer
   configuration" below — band edges live in the same JSON config).
3. **Explicitly run that band's *tune* profile up-sequence first, before
   touching the AH-4 at all** — assert TX INHIBIT, step through whichever
   stages the tune profile includes for this band (may stop at `SEQ2`,
   skipping `SEQ3` — see "Two profiles per band" under Sequencer above),
   release TX INHIBIT — using the same function the STBY interrupt handler
   calls, but with the tune profile rather than the normal-TX profile.
   - **This band's STBY interrupt handling must be suppressed for the
     duration of the tune cycle.** The idempotency argument used elsewhere
     (calling "run up-sequence" again on an already-sequenced band is a
     harmless no-op) only holds when both callers agree on the same
     profile. Here they don't: if the radio's own STBY line asserts once
     PTT goes active and the interrupt handler fires using the *normal*
     profile, it would engage `SEQ3` anyway — exactly what the tune
     profile exists to prevent. So the tune cycle needs to mark this band
     as "under explicit tune-profile control" and have the STBY handler
     defer to it (skip its own trigger) until the tune cycle releases
     that band again in step 9.
   - **Ordering also matters here**: this step is deliberately done
     *before* issuing the AH-4 `START` signal (step 5), not interleaved
     with it.
     The AH-4 has its own tight timing — it asserts `KEY` about 300ms
     after `START`, then expects the carrier almost immediately so it can
     check the power — and the sequencer's relay-settling delay is
     user-configurable and could easily be longer than that. Running the
     sequencer to completion first, while the radio is still fully idle,
     keeps that variable delay out of the AH-4's time-sensitive window
     entirely, so `START → KEY → PTT-key → RF appears` stays a tight,
     undisturbed sequence exactly as the AH-4 expects.
4. Take over the CAT link to the radio and select AM mode (there is no CAT
   power-set command on this radio — switching to AM mode is itself the
   main power reduction, since the FT-847 caps AM output much lower than
   SSB/CW/FM; see the CAT reference below). **The target is about 10W, not
   the minimum:** the AH-4 measures the carrier during the tune and aborts
   if it is outside 5–15W (see the AH-4 reference below). AM mode alone
   still allows up to 25W on HF, so the radio's RF power setting and/or the
   ALC injection circuit must bring the carrier into that window.
   Also claim the CAT bus now (hold PC traffic) so that PTT can go out the
   instant `KEY` asserts, rather than queueing behind a PC poll.
5. Assert `START` (pull it low via its opto), hold it for about 560ms like
   an Icom radio does, then release it. **Key the radio's PTT via CAT right
   away, at `START`, not later at `KEY`.** The radio's carrier overshoots to
   2–3× its set power for ~0.7s after keying and an Alinco EDX-2 that is only
   given RF at `KEY` measures that overshoot, gives up after ≈350ms and
   leaves the SWR >3:1; keyed at `START` the carrier has settled by the time
   `KEY` asserts (≈590ms) and the tune runs 3–4s and ends near 1:1. The cost
   is a few hundred ms of carrier into the unmatched antenna before `KEY`,
   which is acceptable at ~10W (a genuine AH-4 would route RF through its
   power divider only after `KEY`; the key-on-`KEY` variant, Serial0 `R`, is
   kept for tuners that need that). Meanwhile, continue forwarding any other
   CAT traffic between PC and radio live (see above) — only mode and PTT/TX
   status queries get answered from the values captured in step 1 instead
   of the truth.
6. Wait for the tuner to assert `KEY` (during the `START` hold on a genuine
   AH-4, ~35ms after `START` is released on the Alinco EDX-2); give up as
   "no ATU" if it never does (and unkey).
7. Wait for the ATU to signal that its tune cycle has completed: `KEY`
   releasing and staying released is success; `KEY` releasing for ~20ms,
   re-asserting for ~200ms, then releasing is the AH-4's "not tuned"
   signal. (A short `KEY` is not necessarily a failure: an EDX-2 with a
   stored match for the frequency finishes quickly, and the match only
   shows in the SWR on the next key-up. The log shows how long `KEY` was
   asserted; the real judge is the SWR afterwards.)
8. Unkey the radio's PTT **immediately on `KEY`'s first release** (the
   tuner never switches its relays under power, and an Icom radio stops
   transmitting the moment `KEY` goes away).
9. Restore the radio's original mode (from the value captured in step 1;
   retried up to 3 times, and a failure is reported loudly on Serial0), then
   release the CAT bus so PC traffic can flow again.
10. **Explicitly run that band's *tune* profile down-sequence** — the
    mirror of step 3, stepping down through whatever stages the tune
    profile actually engaged. Once complete, release this band from
    "under explicit tune-profile control" so its STBY interrupt handling
    resumes normally. (ALC injection, if used, is released here too; it is
    not wired into the cycle yet.)
11. Resume normal passthrough with no faking.

#### Implementation notes (`src/tune.h`, `src/tune_io.cpp`)

- **One tail for every outcome.** Success, refusal, failure, button abort and
  the 30s watchdog all unwind through the same tail — PTT off (retried until
  the radio has been told), restore mode, release the bus, sequencer down,
  drop the STBY hold, wait for START to finish its hold — so no path can
  skip a cleanup step. It is a pure state machine tested against a fake
  radio and a fake tuner (both an EDX-2-style and a genuine-AH-4-style
  timing) with failures injected at every step and an abort swept across the
  whole cycle.
- **The cycle owns the CAT bus** from before `START` until the restore: the
  arbiter's claim lets PTT go out without queueing behind PC traffic. The PC
  is not locked out meanwhile — it is answered from a snapshot (see "PC
  transparency during a tune cycle"). A claim also releases itself after 60s
  so the PC can never be locked out.
- **Refusals** (nothing is left changed): a band already transmitting, the
  radio reporting it is transmitting, a frequency in no band, a band the AH-4
  isn't enabled for (the config's per-band `atu` flag), no usable
  answer to a CAT query (including a mode byte that couldn't be restored).
- **Tune indicator (D7, a passive buzzer):** driven at ~2048Hz by Timer4's
  hardware PWM (not `tone()`, which would take Timer2 from the ALC pump). A
  continuous tone while a cycle runs; three slow beeps on success; fast
  beeping for about a second on failure or refusal; silent after an abort.
  A low-resistance magnetic buzzer must not be driven straight from the pin
  (a 40Ω coil would draw ~125mA against the Mega's 40mA absolute maximum): put
  about **330Ω** in series (~13mA peak, moderate volume; 1kΩ is very quiet,
  not below ~220Ω), optionally a 1N4148 across the buzzer (cathode to the
  pin side), or use a transistor for more volume. `BUZZER_DUTY_PERCENT` in
  `src/buzzer.cpp` is a software volume control.
- **Controls:** the tune button starts a full tune and, pressed during one,
  aborts it. Serial0 keys: `T` full tune, `E` ATU handshake with the radio
  *not* keyed, `D` dry run (sequencer and AM mode only — no ATU, no RF), `M`
  full tune that also logs the radio's PO/ALC meter (about every 60ms while
  keyed; a diagnostic that can delay PTT off by up to ~100ms, so use it at low
  power only), `P` carrier test (keys the radio for 2s with *no* tuner and
  logs the meter — shows the radio's own start-up behaviour on its own), `R`
  full tune that keys the radio at `START` instead of at `KEY` (so the radio's
  start-up overshoot, below, has settled before the tuner measures), `1`/`2`/`3`
  tune mode AM/FM/CW (default AM), `X` abort. The FT-847 cannot report SWR over CAT, and its status byte is a
  5-bit bar-graph value (not calibrated watts), so SWR and real power need
  external meters.
- **Staged bench procedure** (dummy load, low power): `D` first (check the
  sequencer steps up with the tune profile, the radio goes to AM and comes
  back), then `E` (START/KEY with the real tuner but no carrier — expect "not
  a real tune"), then `T` into a dummy load with the radio's power set for
  about 10W, then onto the antenna.

### Reset safety (watchdog and crash recovery)

A reset puts every pin back to an input, so the optos and LEDs go off and TX
INHIBIT is released, but a CAT PTT stays on until the radio is told otherwise.
So a reset in the middle of a tune could leave the radio keyed and in AM. Two
mechanisms cover this (`src/watchdog.h`, `src/recovery.h`):

- **Watchdog, 2s, interrupt-then-reset.** The main loop feeds it every pass
  (and the slow EEPROM writes feed it too). If the loop stalls, the first
  expiry runs an interrupt that writes a note to EEPROM with the part of the
  loop that was running, and the second expiry resets the board. At the next
  boot, `WATCHDOG: the previous run stalled ... (last in: <stage>)` is
  printed. The note is needed because the Arduino bootloader clears the
  reset-cause register, so the cause of a reset can't be read directly. (The
  stock Mega bootloader handles a watchdog reset correctly - it disables the
  watchdog and jumps straight to the application - but a clone may differ; see
  the bench test below.)
- **Crash recovery.** Just before the tune cycle first changes the radio (the
  AM mode, then PTT) it records "tune in progress, original mode X" in EEPROM
  (3 bytes, magic and complement so a torn or blank record never counts); it
  clears it only once the radio is unkeyed and back in its own mode. If the
  record is still set at boot, the last run did not finish — whatever the
  cause: watchdog, brown-out, USB reset or power loss — so before anything
  else uses CAT the firmware sends CAT on, PTT off, the saved mode, PTT off
  again, clears the record, and prints `RECOVERY: ...`. It never sends PTT
  *on*, and does nothing after an ordinary reset, so a PC's own transmission
  is never cut off.
- **TX INHIBIT at boot.** If a STBY line is already low when the board comes
  up (it rebooted mid-transmission), TX INHIBIT is asserted straight away, not
  when the sequencer gets going.

Bench test (dummy load, low power): 1. Send `!` on Serial0: it stalls the loop
on purpose; the board should reset about 4s later and report the watchdog. If
it instead reboots forever, the bootloader does not handle watchdog resets:
unplug USB to recover and don't use the watchdog (tell me). 2. Start a tune
(`T`) and send `!` while the radio is keyed: after the reset the radio should
come off the air, return to its original mode, and the log should show
`RECOVERY:` - expect a few seconds of carrier first (stall 2s + reset 2s).

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
anything unusual is happening. The Arduino owns the radio's bus for the whole
cycle (the PC and the Arduino cannot both talk to it), so rather than forward
the PC's traffic it keeps reading the PC and answers for the radio, from a
snapshot taken at the start of the cycle (`CatBridgeCore`, `src/cat_bridge_core.h`):

- **Queries are answered at once from the snapshot**, within a few
  milliseconds: freq/mode (`0x03`) gets the real frequency and the
  *original* mode — never AM; TX status (`0xF7`) says "not transmitting";
  RX status (`0xE7`) gives the pre-tune value. The snapshot comes from the
  cycle's own queries (the radio's state before keying), so it works the
  same with no PC history at all.
- **PTT commands (`0x08`, `0x88`) are swallowed** — the PC never sees an
  error (they have no reply) and cannot key or unkey the radio against the
  tune. A PC PTT sent during a tune is not replayed afterwards either.
- **Every other command is queued and replayed after the tune**, in order,
  once the original mode is restored: a mode or frequency change made in
  flrig mid-tune still takes effect, applied last, rather than being lost.
  A query that can't be answered from the snapshot (a satellite VFO, say) is
  queued the same way and answered live afterwards. The queue holds 8
  commands; if it overflows the *oldest* is dropped, so the newest request
  (the latest mode or frequency) always survives.
- **Mode changes are seamless.** At the start, the claim waits for any PC
  exchange already in flight to finish (so nothing is cut in half), and for
  the first few hundred milliseconds — until the snapshot exists — PC bytes
  simply wait in the port's buffer. At the end the queue is replayed (50ms
  after a command, 10ms after a query's reply) and any new PC command goes
  straight through if nothing is queued ahead of it, else queues behind so
  order is preserved. A command split across any of these moments is never
  cut in half (tested by sweeping a split frame across a whole cycle).
- Once the cycle completes, the radio is restored and the queue has drained,
  the Arduino is back to byte-transparent passthrough.

Bench: with the CAT frame log on (`c`), `PROXY>PC` is a reply made up from
the snapshot, `PC swallowed` a dropped PTT, `PC queued` a held command and
`REPLAY>RADIO` its replay.

### Sequencer

A 4-band amplifier/relay sequencer. It runs primarily off the radio's
STBY jack, not CAT, so it works identically whether the PC or even the
CAT link is present at all — but its up/down-sequence logic is also
called directly and proactively by the ATU tune cycle (see above), since
a CAT-commanded PTT does assert STBY (bench-verified) but its lead time
before RF is unmeasured. Both trigger paths call the same up/down-sequence
function, which is safely re-entrant/idempotent **as long as both callers
are applying the same profile**. They aren't always: a tune cycle
deliberately uses each band's "tune" profile (which may skip stages like
`SEQ3` — see below), while the STBY-driven path always applies the
"normal" profile. So while a tune cycle holds explicit control of a
band, that band's STBY-driven triggering must be suppressed rather than
left to run alongside it — see the Tune cycle steps above for exactly
when that hold starts and ends.

- Each band (HF, 50, 144, 430MHz) has its own STBY input line (closure to
  ground = that band's TX requested) and its own independent 5-output
  sequence channel: `RX`, `SEQ1`, `SEQ2`, `SEQ3`, `TX`. All 5 drive a
  status LED; only `SEQ1`/`SEQ2`/`SEQ3` also drive a dedicated
  opto-isolator — `RX`/`TX` are indicator LEDs only, with no relay/opto
  function — giving 12 opto-isolator outputs total, 3 per band.
- **The outputs are cumulative, not mutually exclusive.** `SEQ1`,
  `SEQ2`, and `SEQ3` latch on in sequence and *stay* on together for the
  whole duration of the transmission — they don't step through as
  separate exclusive states. `RX` and `TX` are boundary-condition
  indicator LEDs: `RX` is lit exactly when `SEQ1` is off (i.e. fully idle)
  and unlit the moment `SEQ1` turns on; `TX` lights only once all three
  `SEQ` stages are on and settled, indicating the radio is clear to
  transmit — it does not replace them.
- A single **TX INHIBIT** output is shared across all 4 bands (the radio
  only ever transmits on one band at a time, so one inhibit line is
  enough) and asserted into the radio's TUNER-port TXINH input.
- **Up-sequence**, triggered the instant a band's STBY line asserts (PTT
  pressed):
  1. Assert TX INHIBIT immediately, to hold off actual RF before the relay
     chain has finished settling.
  2. Immediately: `SEQ1` on, `RX` LED off.
  3. After a delay: `SEQ2` on (`SEQ1` stays on).
  4. After a delay: `SEQ3` on (`SEQ1`+`SEQ2` stay on).
  5. After a delay: `TX` LED on — `SEQ1`/`SEQ2`/`SEQ3` are all still
     energised at this point and remain so for the whole transmission.
  6. Release TX INHIBIT so the radio can actually transmit.
- **Down-sequence**, triggered when that band's STBY line de-asserts (PTT
  released), the exact mirror image: `TX` LED off immediately, then after
  a delay `SEQ3` off, then `SEQ2` off, then finally `SEQ1` off together
  with `RX` LED on — `SEQ1`/`SEQ2` stay energised throughout the early
  part of the down-sequence, only dropping out one at a time in reverse
  order.
- **Two profiles per band: "normal TX" and "tune."** A band's step list
  isn't necessarily the same for both. Motivating example: if `SEQ3` is
  wired to a high-power amplifier, engaging it during an ATU tune cycle
  could overload the tuner (which is only ever seeing AM-mode, possibly
  ALC-reduced power — see the ATU tune cycle above — not the amp's full
  output). The **tune** profile for that band would skip `SEQ3` entirely,
  with TX INHIBIT released once `SEQ2` has settled instead of waiting on a
  stage that's deliberately not being engaged. This is per-step
  (`SEQ1`/`SEQ2`/`SEQ3` each independently flagged "included in tune
  profile?"), not hardcoded to skipping `SEQ3` specifically — a different
  band/installation might need to skip a different stage, or more than
  one. Configured via the same JSON config as everything else below.
  Cross-band trigger rules (see "Sequencer configuration" below) are
  assumed to fire the same way regardless of which profile is active —
  RF is present during tuning too, just at reduced power, so protection
  like the masthead-preamp example should still apply. That's a stated
  default, not yet independently confirmed as the right call.
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
  section below for the full reasoning. Bench-verified: CAT keeps working
  with this wiring (see "Bench results").

### Sequencer configuration and cross-band triggers

The 4 bands' sequencers don't have to be fully independent — the design
needs to support one band's transmit event partially triggering another
band's sequencer, configured rather than hardcoded.

- **Motivating example**: transmitting on 50MHz should be able to assert
  the 144MHz channel's `SEQ1` output on its own — e.g. to disable a
  masthead preamp on 144MHz that would otherwise be desensitised or
  damaged by nearby 50MHz RF — without running the rest of the 144MHz
  sequence (`SEQ2`/`SEQ3`/`TX`), since the radio isn't actually
  transmitting on that band.
- **Config-driven, not hardcoded**: defined via a JSON config, loaded over
  the Serial0 USB debug link at runtime rather than from an SD card — this
  was a deliberate choice, since an SD-card approach would need the Mega's
  hardware SPI bus, which the sequencer's Band 4 outputs (D50-D53) have
  already claimed as plain digital pins. Loading over serial avoids that
  conflict and needs no extra hardware.
- **Persisted to EEPROM**, so it survives a power cycle without needing to
  be resent every boot. The Mega's 4KB of EEPROM is comfortably enough for
  this config. If EEPROM is empty or fails validation (e.g. first boot, or
  corrupted data), the sequencer falls back to safe built-in default
  timing with no cross-band trigger rules active, rather than refusing to
  run.
- **Per-band step timing lives in the same config**: the delay between
  each of `SEQ1 → SEQ2 → SEQ3 → TX` (`SEQ1` itself comes on immediately) (and its mirror on the way down)
  is configurable per band via this same JSON, not a hardcoded constant —
  consistent with the whole reason a runtime config exists at all, and
  lets relay timing be tuned per amplifier without reflashing.
- **Cross-band trigger rules are kept deliberately simple**: a rule maps a
  source band to a target band + output (e.g. "50MHz → 144MHz `SEQ1`").
  The trigger point is always the source band's own `SEQ1` turning on
  (the earliest point in its up-sequence, maximising protection margin
  before RF appears) and release is always the mirror — source `SEQ1`
  turning off on the way down. The target output snaps on/off immediately
  when triggered; it does not phase in on its own band's normal step
  delay. Rules only ever switch an output *on*: several rules aimed at the
  same output, or an output the target band's own sequence already has
  on, simply OR together — a rule can never switch off something the
  target's own sequence is holding. The target band's RX LED is untouched
  (the band isn't transmitting). No per-rule timing/trigger-stage options — if finer control turns
  out to be needed later, that's a schema extension, not a redesign.
- **Band-edge frequency ranges also belong in this config**, not
  hardcoded: the ATU tune cycle needs to map "current frequency" to one of
  the 4 sequencer bands (see Tune cycle above), and exact sub-band edges
  can vary by license class/region — so, consistent with everything else
  here, they're configurable rather than baked into firmware.
- **Concrete schema**: see [`config/sequencer.json`](config/sequencer.json)
  for a full worked example (including the 50MHz→144MHz preamp-protection
  rule above). Top level: `schema_version` (lets firmware reject/fall back
  on a shape it doesn't understand), `bands` (keyed `"HF"`/`"50M"`/
  `"144M"`/`"430M"`, each with `freq_min_hz`/`freq_max_hz`, a `timing_ms`
  object with the three up-sequence step delays (`seq1_to_seq2`, `seq2_to_seq3`, `seq3_to_tx`) — the down-sequence mirrors
  these, so there's nothing separate to configure there — and a
  `tune_profile` object of `seq1`/`seq2`/`seq3` booleans, and an optional
  `atu` boolean saying whether the tune cycle may run on that band — absent
  means on for HF and 50M and off for 144/430MHz, the coverage of an AH-4; an
  Alinco EDX-2, which cannot tune 50MHz, sets `"atu": false` there, as
  `config/sequencer-bench.json` does), and
  `cross_band_triggers` (an array of `{source_band, target_band,
  target_output}`). The example file's band edges are placeholder IARU
  Region 1 values — adjust for actual license/region.
- **Transport protocol, over Serial0**: PC sends `CONFIG\n`. Firmware
  responds `READY\n` and switches into "awaiting JSON" mode. The PC streams
  the JSON object; the firmware collects it without blocking (the sequencer
  and CAT bridge keep running), dropping whitespace outside strings as it
  arrives, and stops at the object's closing brace — no end-marker needed.
  The collected text may be at most 1024 characters (whitespace excluded),
  and the upload is abandoned with an error if the port goes quiet for 3
  seconds. The firmware then validates it (structure, types, band edges
  ordered and non-overlapping, timings 0–5000ms, at most 8 trigger rules),
  applies it, writes it to EEPROM, and replies `OK\n` — or
  `ERROR: <reason>\n`, in which case the previous config stays in force.
  `CONFIG` is refused with `ERROR: busy...` (instead of `READY`) while any
  band is transmitting, so a config change can't disturb a live
  transmission (and again at the end, if a band started while the upload was
  arriving: nothing is applied or saved). The handshake line must be exactly
  `CONFIG` (CR or LF); a half-typed line nobody finishes within 1s is
  forgotten, so the leftover of a failed upload can't spoil the next attempt.
  Every way an upload can go wrong ends in an `ERROR:` reply with the previous
  config untouched and the receiver idle again: a first character that isn't
  `{`, nesting deeper than 8, more than 1024 characters of JSON (whitespace not
  counted), a 3s stall (a cable pulled mid-upload), invalid JSON, or valid JSON
  that isn't a usable config (reason given). The logic is in
  `src/config_receiver.h`, a pure class tested with a fake host, including
  randomised fuzzing (random bytes and random whitespace/chunking, run under
  the address and undefined-behaviour sanitizers). Outside that handshake Serial0 behaves exactly as it always
  does (debug logging, etc.) — this is a small carve-out, not a separate
  mode that changes anything else about the port.
- **EEPROM image**: magic, layout version, length, a field-by-field
  little-endian payload (the per-band `atu` flags sit at the end, so images
  saved before they existed still load, with the default coverage) and a
  CRC-16, so padding or compiler changes can't
  matter, and any corruption (or erased EEPROM) is rejected. A loaded
  image is re-validated with the same rules as an uploaded config.
- **Built-in fallback**: if EEPROM is empty or invalid, the firmware runs
  from a compiled-in table with the band edges, timing and tune profiles
  of `config/sequencer.json` and **no** cross-band triggers; a native test
  keeps the table in step with that file. `config/sequencer-bench.json` is
  the same config with every timing slowed to 1000ms, for easy visibility
  on the bench.
- **PC-side helper**: [`tools/send_config.py`](tools/send_config.py) —
  validates the JSON is well-formed *before* touching the serial port
  (fails fast with a clear Python error rather than a round-trip to the
  device), waits out the Mega's reset on port open, then runs the
  handshake above (skipping any log lines the firmware prints in between)
  and prints the Arduino's response. Makes a config change a one-command action:
  `python3 tools/send_config.py config/sequencer.json /dev/ttyACM0`.
  Requires `pip install pyserial`.

### Debug/control port

Serial0 (USB) runs at **115200 baud** — deliberately faster than, and
independent of, the CAT link's 57600 (Serial2/Serial3 are constrained to
match the radio's CAT-rate menu; Serial0 is a USB virtual serial port with
no such constraint). Available for logging Arduino/CAT activity and for
local control of the sequencer, independent of the CAT passthrough path.
This is also the transport for loading the sequencer's JSON config (band
timing, cross-band trigger rules) at runtime — see "Sequencer
configuration and cross-band triggers" above.

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
  reduction** — there is no separate power-set step. But AM mode on its own
  is not enough for the AH-4, which needs a carrier of 5–15W (about 10W)
  and aborts outside that: 25W AM exceeds it, so the radio's RF power
  setting and/or the ALC injection must trim it into the window.
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

Sources: `docs/AH-4_Design_and_Operation.pdf` (K9EQ, "Inside the Icom
AH-4"; abbreviated "K9EQ" below), `docs/Icom AH4 SGC Tuner Protocol
Converter.pdf` (captured timing diagrams for the genuine Icom AH-4
protocol) and [doumae/fakeFC](https://github.com/doumae/fakeFC) —
[`fakeFC.ino`](https://github.com/doumae/fakeFC/blob/9eb30efeaafe0d38e46a250920fb501777e3a7f1/fakeFC.ino)
(an Arduino sketch that drives a real AH-4 from a Yaesu tuner port).

- **Physical interface**: a 4-wire connector — `START` (input to the
  tuner), `KEY` (output from the tuner), `+13.8V`, `GND`. Both `START` and
  `KEY` are **active-low** (the SGC PDF calls them `START_L` / `KEY_L`): a
  line is "asserted" by pulling it to ground, and idles high. The tuner
  draws under 300mA typically and under 1A peak.
- **What the tuner does** (K9EQ): the microprocessor is powered down except
  while tuning. `START` asserted resets it, and it is running about 300ms
  later. It then asserts `KEY` and routes RF through a 10:1 power divider,
  measurement circuit and the tuning network, so the radio sees a low SWR
  (about 350mW reaches the tuner). `KEY` is what makes an Icom radio
  transmit a carrier of about **10W**. The AH-4 checks that the power is
  **between 5W and 15W** and aborts the tune if it is not. About 250ms
  after it starts tuning the radio releases `START`. When tuned, the AH-4
  releases `KEY` and switches RF to pass through the tuning network only;
  the radio stops transmitting when `KEY` goes away. The tuning relays are
  never switched under power. If the AH-4 cannot tune, it releases `KEY`
  for 20ms, asserts it again for 200ms, then releases it for good, which
  the radio reports as "not tuned". A band change on the radio resets the
  tuner (tuning network out of circuit).
- **Timing — the sources differ, so the firmware is conservative to all
  of them and the real tuner should be measured** (the bench key `h` logs
  when `KEY` asserts). Measured on an **Alinco EDX-2** (Icom-compatible
  tuner), with no RF applied: `KEY` asserts **≈35ms after `START` is
  *released***, however long `START` was held — `START` held 800ms gave
  `KEY` at 833ms and held 2000ms gave 2036ms (repeatable to ±2ms) — then
  stays asserted for ≈326ms and releases once, with no re-assert. So that
  tuner acts on `START`'s trailing edge, which a driver that holds `START`
  until `KEY` appears can never satisfy.

  | | SGC capture / fakeFC | K9EQ |
  |---|---|---|
  | `KEY` asserts after `START` | ~10ms after `START` has been held >100ms (≈110ms) | ≈300ms (figure 4 shows ≈250–310ms) |
  | `START` released | by the controller after `KEY` releases | by the radio ≈250ms after tuning begins (`START` low ≈560ms in total), while `KEY` is still asserted |
  | Alinco EDX-2 (measured) | — | `KEY` ≈35ms after `START` is released; no re-assert, `KEY` ≈326ms with no RF |
  | `KEY` asserted for | up to ~2–2.5s | 560ms – 2s typical |
  | Failed tune | `KEY` re-asserts within ~25ms of releasing | `KEY` low, released 20ms, asserted 200ms, released |
  | Short `START` | <~100ms = bypass/toggle request | ~70ms pulse with no `KEY` activity = **tuner reset** (pass-through, tuning network out) |

- **Practical sequence for this project** (Arduino driving a real AH-4
  directly, not through the Yaesu tuner-port protocol fakeFC emulates).
  The driver (`src/ah4.h`) behaves like an Icom radio and accepts either
  tuner's timing:
  1. Pull `START` low and hold it for 560ms (K9EQ: an Icom radio's `START`
     is low about that long), then release it, whatever `KEY` is doing.
     It is never released earlier than 150ms after asserting (also on
     abort), since a `START` pulse of ~70–100ms is the tuner's reset
     command.
  2. Accept `KEY` asserting anywhere from `START` asserting to 500ms after
     `START` is released (a genuine AH-4 asserts it during the hold; the
     EDX-2 ~35ms after release); if it never does there is no tuner
     responding (error case). When to key the radio is the tune cycle's
     choice — see the tune cycle above: at `START` by default, so the
     radio's start-up overshoot has settled by the time `KEY` asserts.
  3. Wait for `KEY` to release again (busy phase, 15s timeout; an EDX-2 tune
     took 3–4s, and the actual duration is logged so this can be tightened).
  4. When `KEY` releases, unkey PTT at once, then keep watching `KEY` for
     50ms. Any re-assertion in that window is the AH-4's failure signature
     (its 20ms gap sits inside the window with margin); `KEY` staying
     released is success.
  5. Restore the radio's prior mode via CAT.
  `KEY` releasing with no RF applied is *not* a tune — the EDX-2 just lets
  go of `KEY` after ~326ms — so the tune cycle, which knows whether it
  actually keyed RF, decides what counts as success.
  A `KEY` already asserted before `START` is refused (stuck line or a
  tuner that is already busy) without touching `START`.
- fakeFC also emulates the separate Yaesu FC-40 tuner-port protocol (a
  different connector/protocol from CAT, using single-byte `0xa0`/`0xa1`
  status codes and `TXINH`/`TXGND` lines) so a Yaesu radio's native tuner
  port can drive a non-Yaesu tuner. That part isn't relevant to us — we are
  driving the AH-4's `START`/`KEY` lines directly from the Arduino and
  controlling the radio purely over CAT, not emulating a Yaesu tuner port.
- **Opto-isolated drive/read circuit**, for galvanic isolation between the
  Arduino's 5V logic and the AH-4's 13.8V circuit. Both opto LED circuits
  are powered from the AH-4 connector's own `+13.8V`/`GND` pins, so no
  separate supply is needed and the isolation boundary stays clean — no
  shared ground between the Arduino and the AH-4/radio side at all.
  - **`START`** (Arduino asserts by pulling it low at the AH-4 end): `D12`
    → ~330-470Ω resistor → opto LED anode → cathode → Arduino GND (drives
    the opto). Opto's phototransistor: collector → AH-4 `START` pin,
    emitter → AH-4-side `GND`. If the tuner has no pull-up of its own, a ~10kΩ
    pull-up from `START` to the AH-4 connector's `+13.8V` provides the
    idle-high level (see below). Opto off → `START` idles high; opto on (D12
    driven high) → phototransistor pulls `START` near 0V (asserted). **Whether the 10kΩ pull-up is needed depends
    on the tuner.** K9EQ describes a genuine AH-4's `START` as pulled up to
    13.8V inside the *radio*, so a genuine AH-4 needs this circuit to
    provide it. An **Alinco EDX-2** does not: its `START` input is the 100µH
    choke and 4k7 to the base of a PNP whose emitter is at 5V, which supplies
    its own pull-up, so there is no pull-up here at all and the collector
    carries under 1mA.
  - **Protect the opto from the cable.** The tuner end of the cable is at the
    antenna, and the opto's transistor can only stand about 6V *reverse*
    (collector below emitter) — a negative spike on `START` can damage it
    while the LED side still works (one opto died this way: LED lit, `START`
    never pulled low, "no ATU: KEY never asserted"). A **1N4148 in series**
    with the collector, anode to the `START` cable and cathode to the opto
    collector (the sink current flows from the line into the collector), blocks
    any negative excursion. It raises the asserted level at the cable to about
    0.8V (transistor ≈0.2V + diode ≈0.6V), still far below the ~4.3V the PNP
    needs. Against *positive* spikes the opto's 35V collector rating is the
    limit; a ~20V transient suppressor from the cable side to ground would add
    margin if it is ever needed.
  - **`KEY`** (AH-4 asserts by pulling it low at its own end): AH-4-side
    `+13.8V` → ~1kΩ resistor → opto LED anode → cathode → AH-4 `KEY` pin
    (the AH-4 completes this loop to its own ground when busy). Opto's
    phototransistor: collector → `D11`, configured `INPUT_PULLUP` (same
    convention as fakeFC's direct-wired approach) → Arduino's own 5V
    internally; emitter → Arduino GND. AH-4 idle → opto off → `D11` reads
    high (pull-up); AH-4 busy → opto on → phototransistor pulls `D11` low.
    (K9EQ: the AH-4's `KEY` is an open-collector transistor to ground, with
    an internal 22kΩ + diode towards 5V, and the radio pulls the line up to
    13.8V through a resistor. The opto LED's series resistor is that
    pull-up here; the AH-4's sink current is `(13.8V − 1.2V) / R`, so a
    larger R (e.g. 2.2kΩ ≈ 5mA) is gentler on the AH-4 than 1kΩ ≈ 12mA,
    and the opto still saturates easily.)
  - A standard low-speed part (e.g. PC817) is more than adequate — these
    are millisecond-scale control lines, nowhere near a PC817's ~µs-scale
    switching time, and its Vceo (~35V) comfortably clears the 13.8V rail.

### ALC in the firmware (`src/alc_io.h`)

Wired: the opto's LED is fed from D10 through 470Ω (about 8mA), its collector
goes to the radio's ALC line and its emitter to the charge pump's negative
output, so the transistor pulls the ALC line towards the pump voltage — the
direction that only ever reduces power. The pump (Timer2, D9, ~14.9kHz) runs
all the time; D10 is the gate.

- **Fail-safe:** a reset or a hang turns the gate off (the pin becomes an input,
  the LED goes dark), a gate switched on by hand releases itself after 30s, and
  the tune cycle releases it as soon as the radio is unkeyed on every path —
  success, failure, abort — which the tests check by breaking things in seven
  ways and sweeping an abort across the whole cycle.
- **Bench key (no tune running):** `g` gate on/off. The pump duty is fixed at 50%:
  on the real circuit 5% to 95% moved the output by only ~0.2V (a diode charge
  pump's voltage is set by its diode drops and the supply, not the duty), so it
  is not a control. To back the voltage off, put a 100kΩ pot in the circuit.
  Opening the walk-test and exiting it restarts the pump (leaving it would have
  stopped it: `configurePins()` disconnects D9's PWM output as a side effect).
- **In a tune:** `L` toggles "ALC injection for tunes" (off by default until it
  is calibrated). When on, the gate goes on once the mode is set, is given
  100ms to settle, and only then does the tuner start and the radio get keyed;
  it is released in the tail right after PTT off.
- **Calibrating:** measure the pump's -V with the gate off, then press `g` and
  watch the radio's ALC/power. With `L` on, run the carrier test (`P`, dummy
  load) and compare the radio's PO meter and your external wattmeter with and
  without ALC: the aim is a steady ~10W with the start-up overshoot (above)
  taken out, adjusting the pot.

- **Status:** the pump and gate work — -4V is measured at the radio's EXT ALC
  jack during a tune — but the radio's carrier does not change with it, so the
  ALC injection stays **off by default** (`L`) and the tune does not depend on
  it: keying at `START` already avoids the start-up overshoot (see the bench
  results). To find out whether the radio sees the voltage at all, set Menu #24
  (TX MTR) to ALC: the manual says the ALC meter reading includes "any external
  ALC voltage", so the meter should deflect when the gate is on while keyed
  (`P` with `L` on, dummy load). If it doesn't move, the voltage isn't reaching
  the radio's ALC circuit; if it does, the radio sees it but isn't acting on it
  in this mode.

## ALC injection for tune power (reference)

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
  alone. The goal is the AH-4's ~10W carrier (it aborts below 5W as well
  as above 15W), so the PWM level is a calibration to set against a power
  meter, not "as low as possible".
- **Relationship to AM mode**: the two are complementary, not
  alternatives. AM mode is switched to regardless (see the CAT reference
  above — the FT-847 offers no other way to run a controlled tune cycle
  over CAT), and the ALC injection is layered on top of that to trim the
  carrier down to about 10W during the keyed window.
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
  Arduino D5 as an input (mirroring fakeFC's own pin choice). It is *not*
  tied to the project's ground rail, precisely because it isn't one.
- **`TX_GND` function confirmed against the actual FT-847 schematic.** It's
  the collector of a **DTC114EK** digital transistor, emitter to ground —
  so it's an **open-collector, active-low** signal: pulled to ~0V when
  asserted, floating high otherwise (confirming why fakeFC wires its
  equivalent pin `INPUT_PULLUP`, and why D5 should be too). The base is
  driven when the radio is transmitting on **HF or 50MHz specifically** —
  which lines up exactly with the FC-20 tuner's actual coverage range
  (1.8-50MHz): this is a band-scoped "TX active on a band the FC-20 cares
  about" status line, not a general TX indicator. Since the STBY jack
  already gives 4 independent, full-resolution per-band lines (HF/50/144/
  430), `TX_GND` doesn't add anything STBY doesn't already cover — if
  anything it's less granular (one combined line for 2 of the 4 bands).
  **Conclusion: not load-bearing for the sequencer or tune cycle** — worth
  logging on Serial0 as a cross-check against the STBY-HF/STBY-50MHz
  lines, but nothing in the core logic needs to depend on it.
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
  should keep working normally. **Bench-verified on this radio** — with only pins 1, 2 and 8 wired
  and Tuner Sense unconnected, CAT still responds on Port 2 (see "Bench
  results").
- **The YT-847 precedent** supports this being a reasonable approach: its
  manual's install steps connect its interface cable to *both* the TUNER
  jack and the CAT jack simultaneously, and CAT still works for it — its
  own text says the TUNER jack is used *only* to draw +12V power from the
  radio, with all actual radio control happening over the separate CAT
  port, never speaking the FC-20 tuner protocol. Same underlying strategy:
  don't engage whatever it is on the TUNER connector that trips the
  interlock.
- **A single-shared-UART mux theory was floated and investigated for *why*
  the interlock exists**: the idea being the FT-847 has one internal
  UART, switched by an analogue mux (e.g. a 4053-style chip) between the
  MAX232/CAT port and the TUNER connector's `DATA_IN`/`DATA_OUT` pins, so
  CAT and the TUNER protocol would be mutually exclusive at the hardware
  level. Re-checking fakeFC's code against this: every single
  `fc_txinh(true)` call in the sketch is sandwiched directly between two
  `Serial1` byte-sends (its "RIG" UART, wired to `DATA_IN`/`DATA_OUT`) —
  never asserted standalone. That's consistent with the mux theory, but
  only shows correlation, not which signal actually drives it.
- **Resolved with high confidence** by G0AFH's article ["How To Sequence
  The FT-847"](http://g0afh.com/how-to-sequence-the-ft847/) (the primary
  source; its content was pasted in directly since the page wouldn't load
  for this session — a TLS/certificate issue on their server), corroborated
  by [kl7uw.com/TX-INHIBIT.htm](https://www.kl7uw.com/TX-INHIBIT.htm) and
  this project's own FT-847 schematic trace:
  - **Why TX INHIBIT exists at all**: the FT-847 has "VOX always on" —
    mic PTT, the front-panel MOX switch, *and* the CW key input can each
    independently put the radio into TX, which is a real risk with a
    masthead preamp or high-power amp in the chain. Routing PTT itself
    through sequencer relay contacts is described as the common but
    imperfect fix (doesn't catch the front-panel MOX switch, and can
    introduce AC hum / break QSK). TX INHIBIT catches all of these paths
    uniformly, which is why it's the right mechanism for this project too.
  - **Polarity confirmed, not just inferred**: "Pulling this signal high
    prevents the radio from generating any RF." Active-high, directly
    stated for this exact radio — matching what fakeFC's firmware
    behaviour had only implied.
  - **The load-bearing fact for this whole sequencer design, confirmed**:
    "Keying the microphone, pushing the MOX, or touching the key will
    still put the radio into TX but no RF will be produced. Usefully the
    PTT lines from the STANDBY socket still work which make interfacing
    to an external sequencer even easier." STBY keeps asserting normally
    regardless of TX INHIBIT state — exactly the assumption the sequencer
    is built on, now confirmed by someone who's actually built and used
    this exact technique on this exact radio.
  - **Reference circuit**: G0AFH's own TX INHIBIT driver is simple — a
    78L05 5V regulator (powered from TUNER pin 1's ~13.5V, matching this
    project's own pin 1 reading), a resistor (shown as 1kΩ, noted as
    "almost certainly" fine up to 10kΩ) and a schottky diode (type
    non-critical — a BAS16 was used) to pull TX-INH high. No opto-isolator
    or special circuitry — consistent with fakeFC's own plain-GPIO
    approach.
  - **Direct 5V drive confirmed by the actual internal part, datasheet in
    hand.** Tracing the FT-847's own schematic further: TX INH feeds into
    the `IN` pin of a **ROHM DTC144E-series digital transistor** (an NPN
    BJT with built-in bias resistors, `R1 = R2 = 47kΩ`) — a part family
    whose entire purpose, per ROHM's datasheet, is "built-in bias
    resistors enable the configuration of an inverter circuit *without
    connecting external input resistors*." Guaranteed thresholds:
    `V_I(on)` ≥ 3.0V, `V_I(off)` ≤ 0.5V, input current ≤ 180µA at 5V —
    directly driving it from D4 (5V logic) with no series resistor, diode,
    or regulator is exactly this part's intended use, and simpler even
    than G0AFH's own reference circuit above. **Confirmed against the
    actual FT-847 schematic**: pin 1 = base (`IN`), pin 2 = emitter
    (`GND`), pin 3 = collector (`OUT`) — matching the
    `DTC144EM`/`DTC144EEB`/`DTC144EUB` pinout variant, not the other one.
    Pin 1 is the input as assumed.
  - This also confirms the earlier mux-theory worry was unfounded: TX INH
    runs through transistors to an internal `KEY` node on the keying
    signal path, not through whatever governs CAT/TUNER-port UART sharing.
- **The bench test is now a confirmation, not a live architectural
  risk.** Still worth verifying CAT keeps responding while TX INHIBIT is
  actively held high, and pinning down its exact polarity/timing against
  the real radio, but this is no longer expected to threaten the broker
  architecture the way it looked like it might.

## Bench results

Verified on the real FT-847 with the test jig (TX INHIBIT, the four STBY
lines and CAT connected; TUNER Sense unconnected):

- **Pin map**: every output and input checked against `src/pins.h`
  (walk-test, Phase 0). LEDs and optos are active-high; `START` idles low.
- **STBY**: the radio pulls the correct band's STBY line low on PTT, on
  all four bands, for both manual PTT and **CAT-commanded PTT** (`0x08`).
- **TX INHIBIT (D4 → TUNER pin 8)**: works as designed. No RF was observed
  between STBY asserting and TX INHIBIT asserting, driving it from the
  main loop (not the ISR) — so the loop is fast enough as things stand.
  No latency figure has been measured.
- **CAT with the TUNER connector wired** (pins 1, 2, 8 only): CAT keeps
  working through the Arduino, including while TX INHIBIT is held during a
  sequence and with flrig polling.
- **Radio carrier start-up overshoot (AM, HF, FT-847)**: keyed alone with
  the RF power set for 10W (steady, on an external meter and the radio's
  display), the radio's PO meter via CAT (`0xF7`, bits 4:0, an uncalibrated
  0–31 bar value) starts at 21–23, falls steadily over about 700ms and then
  sits flat at 8. So the carrier overshoots to roughly 2.5–3× its steady
  level and takes ~0.7s to settle (a slow external meter hides this).
- **Tune cycle on an Alinco EDX-2 (HF, AM, ~10W into the antenna, external
  SWR meter)**: keyed at `KEY` it released `KEY` after ≈350ms (no longer
  than with no RF at all) with the SWR still >3:1, even though the tuner was
  seen starting to switch relays. **Keyed at `START` instead** it holds `KEY`
  for 3.1s and 4.0s (two runs, 18.14MHz and 14.3MHz) and the SWR ends close
  to 1.0 — a real tune, and the radio's original mode (AM already, and CW)
  restored afterwards. The default tune now keys at `START`.
  The EDX-2 appears to have tuning memories, so a repeat tune on a frequency
  it already knows can finish in well under a second, and it only switches
  its matched network in after `KEY` releases and the radio has unkeyed, so
  the SWR still reads high as the cycle ends and is ~1.0 on the next key-up.
  Along the way: `KEY` asserts ≈31ms after `START` is released (any hold
  length), and spurious STBY pulses on the 430M line while HF transmitted
  (probably RF pickup) are why every band's STBY is held during a tune.
- **CAT bridge**: transparent passthrough to flrig over an extended
  session with no stray, torn or timed-out frames.
- **Arduino-originated CAT** on Port 2 (freq/mode, TX status, set mode,
  PTT) works standalone and alongside a PC polling at full rate.

## Open questions / to be determined

- **How soon after `KEY` the tuner expects RF**, and what it does with RF
  present (the failure signature, the tune length, whether the EDX-2
  repeats the 20ms/200ms pattern or something else). `KEY`-to-RF latency
  through the CAT PTT path (arbiter quiet gap, radio response) has to land
  well inside the tuner's window. Needs a keyed test into a dummy load.
- **Tune carrier power**: the AH-4 aborts outside 5–15W, and the radio in
  AM mode can do 25W. Calibrate the RF power setting and ALC level for
  about 10W on a power meter. The transmit-status meter bits (`0xF7`,
  bits 4:0) might allow a sanity check from the Arduino.
- **Tuner reset on band change**: Icom radios reset the AH-4 (a ~70ms
  `START` pulse) when the band changes so a stale tuning network isn't
  left in circuit. Nothing here does that yet; the band is known at each
  PTT from STBY, so resetting when it differs from the last tuned band is
  possible.
- Debounce/timing behaviour of the physical tune button (D6), exact
  blink/status patterns for the tune LED (D7), and what the generic
  activity LED (D8) should actually indicate.
- **Implementation mechanism for "suppress this band's STBY handling
  while a tune cycle holds it"** — likely a simple per-band flag/state the
  interrupt handler checks before acting, but needs designing alongside
  the sequencer's core state machine so it can't be forgotten or race
  against the interrupt firing at an inconvenient moment.
- Whether cross-band trigger rules should really apply unconditionally
  regardless of tune-vs-normal profile (the stated default above), or
  whether that too should be configurable per rule.
- **STBY-to-RF lead time** (manual and CAT PTT) hasn't been measured; no
  RF was seen leaking before TX INHIBIT asserted, but there's no figure
  yet. Needs a scope capture of STBY falling against D4 rising and RF
  onset.
