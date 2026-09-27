# Yaesu/Icom ATU Sequencer v2

An Arduino Mega 2560 project that sits transparently between a PC running CAT
control software (e.g. `flrig`) and a Yaesu FT-847 transceiver, so it can
automatically sequence an Icom automatic antenna tuner (e.g. AH-4) without the
PC being aware that anything happened.

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
- **Serial1 ("Port 1")**: connects to the PC's CAT serial port, via one half
  of a MAX202CPE (RS-232 level shifter).
- **Serial2 ("Port 2")**: connects to the Yaesu FT-847's CAT port, via the
  other half of the MAX202CPE.
- **Icom ATU interface**: drives an Icom-series automatic antenna tuner (e.g.
  AH-4) using its start/key line(s) and tune-complete signalling.
- **Tune button**: a physical pushbutton wired to an Arduino input, used to
  initiate a tune cycle.

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
   - Set RF power to minimum.
   - Select AM mode.
   - Key the radio's PTT.
3. Wait for the ATU to signal that its tune cycle has completed.
4. Unkey the radio's PTT.
5. Restore the radio's original power level and mode (from the cached
   pre-tune state).
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

## Open questions / to be determined

- Exact Yaesu FT-847 CAT command set and framing to intercept/emulate
  (power, mode, PTT get/set opcodes) — to be researched when we build the
  CAT layer.
- Icom ATU interface start sequence and tune-complete signalling (voltage
  levels, timing, which pins) — to be researched when we build the ATU
  sequencing code.
- Serial baud rates/framing for each port (PC↔Arduino may differ from
  Arduino↔radio).
- Debounce/timing behaviour of the tune button, and whether a tune-in-progress
  indicator (LED) is desired.
