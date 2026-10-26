#!/usr/bin/env python3
"""Read the LXR-02 status and audio CPU load over SysEx (docs/sysex.md 5.4).

Usage:
  python3 tools/lxr_status.py --list
  python3 tools/lxr_status.py [--port NAME] [--device N] [--reset] [--watch SECONDS] [--json]

  --port    MIDI port name (substring match, used for input and output).
            Default: first port whose name contains "LXR".
  --device  SysEx device ID 1-16, default 0 = broadcast (dd = 7F).
  --reset   clear the CPU peak and underrun counter after the reply
            (STATUS_REQUEST flag bit 0).
  --watch   poll every SECONDS until Ctrl-C.
  --json    print one JSON object per reply.

Needs `pip install mido python-rtmidi` for the MIDI I/O. The frame helpers
(build_status_request, parse_status_reply) have no dependencies and are
covered by tools/test_lxr_status.py.

SPDX-License-Identifier: GPL-3.0-only
"""

import argparse
import json
import sys
import time

MANUFACTURER_ID = 0x7D
MODEL_ID = 0x4C
PROTOCOL_VERSION = 0x01
CMD_STATUS_REQ = 0x03
CMD_STATUS_REPLY = 0x04
STATUS_RESET_PEAK = 0x01

CPU_FIELDS = ("cpu_now", "cpu_peak", "cpu_voices", "cpu_mixer", "cpu_fx", "underruns")


def checksum(data):
    c = 0
    for b in data:
        c ^= b
    return c & 0x7F


def build_status_request(device=0x7F, reset=False):
    """Returns the complete STATUS_REQUEST frame (F0 ... F7) as bytes."""
    body = [device & 0x7F, PROTOCOL_VERSION, CMD_STATUS_REQ]
    if reset:
        body.append(STATUS_RESET_PEAK)
    return bytes([0xF0, MANUFACTURER_ID, MODEL_ID] + body + [checksum(body), 0xF7])


def parse_status_reply(frame):
    """Parses a STATUS_REPLY frame (with or without F0/F7).

    Returns a dict, or None if the frame is not a valid STATUS_REPLY.
    Loads are in percent (float). `has_cpu` is False for firmware without the
    CPU meter; then the CPU keys are missing.
    """
    data = list(frame)
    if data and data[0] == 0xF0:
        data = data[1:]
    if data and data[-1] == 0xF7:
        data = data[:-1]
    if len(data) < 6 or data[0] != MANUFACTURER_ID or data[1] != MODEL_ID:
        return None
    dev, ver, cmd = data[2], data[3], data[4]
    if cmd != CMD_STATUS_REPLY or checksum(data[2:-1]) != data[-1]:
        return None
    payload = data[5:-1]
    if len(payload) < 4:
        return None
    out = {
        "device": dev,
        "version": ver,
        "kit": payload[0],
        "pattern": payload[1],
        "running": bool(payload[2]),
        "echo": bool(payload[3]),
        "has_cpu": len(payload) >= 4 + 2 * len(CPU_FIELDS),
    }
    if out["has_cpu"]:
        for i, name in enumerate(CPU_FIELDS):
            v = payload[4 + 2 * i] | (payload[5 + 2 * i] << 7)
            out[name] = v if name == "underruns" else v / 10.0
    return out


def format_status(st):
    line = "kit %d  pattern %d  %s" % (st["kit"], st["pattern"], "running" if st["running"] else "stopped")
    if not st["has_cpu"]:
        return line + "  (firmware has no CPU meter)"
    return line + "  |  cpu %5.1f%%  peak %5.1f%%  voices %5.1f%%  mixer %5.1f%%  fx %5.1f%%  xruns %d" % (
        st["cpu_now"], st["cpu_peak"], st["cpu_voices"], st["cpu_mixer"], st["cpu_fx"], st["underruns"])


def _find_port(names, wanted):
    for n in names:
        if wanted.lower() in n.lower():
            return n
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", default="LXR")
    ap.add_argument("--device", type=int, default=0, help="1-16, 0 = broadcast")
    ap.add_argument("--reset", action="store_true")
    ap.add_argument("--watch", type=float, default=0)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--timeout", type=float, default=0.5)
    args = ap.parse_args(argv)

    try:
        import mido
    except ImportError:
        sys.exit("needs mido: pip install mido python-rtmidi")

    if args.list:
        print("inputs: ", mido.get_input_names())
        print("outputs:", mido.get_output_names())
        return 0

    in_name = _find_port(mido.get_input_names(), args.port)
    out_name = _find_port(mido.get_output_names(), args.port)
    if not in_name or not out_name:
        sys.exit("no MIDI port matching %r (use --list)" % args.port)

    device = 0x7F if args.device == 0 else args.device - 1
    with mido.open_input(in_name) as inp, mido.open_output(out_name) as outp:
        while True:
            req = build_status_request(device, args.reset)
            outp.send(mido.Message("sysex", data=list(req[1:-1])))
            st = None
            deadline = time.monotonic() + args.timeout
            while st is None and time.monotonic() < deadline:
                msg = inp.poll()
                if msg is None:
                    time.sleep(0.002)
                elif msg.type == "sysex":
                    st = parse_status_reply(bytes(msg.data))
            if st is None:
                print("no STATUS_REPLY (is SysEx enabled on the unit?)", file=sys.stderr)
            elif args.json:
                print(json.dumps(st), flush=True)
            else:
                print(format_status(st), flush=True)
            if not args.watch:
                return 0 if st else 1
            time.sleep(args.watch)


if __name__ == "__main__":
    sys.exit(main())
