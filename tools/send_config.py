#!/usr/bin/env python3
"""Push config/sequencer.json to the Arduino over its Serial0 debug link.

Usage: send_config.py [path/to/config.json] [serial-port]
Defaults: config/sequencer.json, /dev/ttyACM0

Requires: pip install pyserial
"""

import json
import sys

import serial

BAUD = 115200
TIMEOUT_S = 5


def main() -> int:
    config_path = sys.argv[1] if len(sys.argv) > 1 else "config/sequencer.json"
    port = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyACM0"

    with open(config_path, "rb") as f:
        raw = f.read()

    # Fail fast on malformed JSON before ever touching the serial port.
    try:
        json.loads(raw)
    except json.JSONDecodeError as e:
        print(f"{config_path} is not valid JSON: {e}", file=sys.stderr)
        return 1

    with serial.Serial(port, BAUD, timeout=TIMEOUT_S) as ser:
        ser.reset_input_buffer()
        ser.write(b"CONFIG\n")

        ready = ser.readline()
        if ready.strip() != b"READY":
            print(f"Unexpected response to CONFIG: {ready!r}", file=sys.stderr)
            return 1

        ser.write(raw)

        result = ser.readline()
        print(result.decode(errors="replace").strip())
        return 0 if result.strip() == b"OK" else 1


if __name__ == "__main__":
    raise SystemExit(main())
