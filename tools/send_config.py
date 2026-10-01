#!/usr/bin/env python3
"""Push a sequencer config (default config/sequencer.json) to the Arduino over
its Serial0 debug link, and report the result.

Usage: send_config.py [path/to/config.json] [serial-port]
Defaults: config/sequencer.json, /dev/ttyACM0

Handshake: send "CONFIG\\n", wait for "READY", send the JSON, wait for "OK" or
"ERROR: <reason>". The firmware also prints log lines on this port (sequencer
transitions etc.), so every reply is looked for among them, not assumed to be
the next line.

Requires: pip install pyserial
"""

import json
import sys
import time

import serial

BAUD = 115200
BOOT_WAIT_S = 2.5  # opening the port resets the Mega (DTR); let the bootloader finish
REPLY_TIMEOUT_S = 5


def wait_for(ser, wanted, timeout):
    """Read lines until one satisfies wanted(line); return it, or None on timeout."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = ser.readline().decode(errors="replace").strip()
        if line and wanted(line):
            return line
    return None


def main() -> int:
    config_path = sys.argv[1] if len(sys.argv) > 1 else "config/sequencer.json"
    port = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyACM0"

    with open(config_path, "rb") as f:
        raw = f.read()

    # Fail fast on malformed JSON before ever touching the serial port.
    try:
        doc = json.loads(raw)
    except json.JSONDecodeError as e:
        print(f"{config_path} is not valid JSON: {e}", file=sys.stderr)
        return 1
    # Compact form: smaller on the wire. The firmware also copes with whitespace.
    payload = json.dumps(doc, separators=(",", ":")).encode()

    with serial.Serial(port, BAUD, timeout=0.2) as ser:
        time.sleep(BOOT_WAIT_S)
        ser.reset_input_buffer()
        ser.write(b"CONFIG\n")

        reply = wait_for(ser, lambda l: l == "READY" or l.startswith("ERROR"), REPLY_TIMEOUT_S)
        if reply != "READY":
            print(reply or "No response to CONFIG (is this the Arduino's USB port?)", file=sys.stderr)
            return 1

        ser.write(payload)

        result = wait_for(ser, lambda l: l == "OK" or l.startswith("ERROR"), REPLY_TIMEOUT_S)
        print(result or "No OK/ERROR reply after sending the config", file=sys.stderr if result != "OK" else sys.stdout)
        return 0 if result == "OK" else 1


if __name__ == "__main__":
    raise SystemExit(main())
