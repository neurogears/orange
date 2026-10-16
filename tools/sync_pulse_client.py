#!/usr/bin/env python3
"""Ask orange for sync pulses over ZeroMQ, the way the Bonsai PC does.

Each request makes orange let one exposure of the sync camera through to the
Harp input (exposure line AND gate line) and reply with that frame's PTP
timestamp (see docs/zmq_sync.md).

    uv run --with pyzmq tools/sync_pulse_client.py tcp://<orange-pc>:5560 \
        --count 10 --interval 2 --csv pulses.csv
    uv run --with pyzmq tools/sync_pulse_client.py tcp://<orange-pc>:5560 --status
    uv run --with pyzmq tools/sync_pulse_client.py tcp://<orange-pc>:5560 \
        --probe 4000 --count 5      # gate open 4 ms after a frame timestamp
"""
import argparse
import csv
import json
import sys
import time

import zmq


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("endpoint", help="orange's sync-pulse endpoint, e.g. tcp://10.0.0.5:5560")
    ap.add_argument("--count", type=int, default=1, help="pulses to request")
    ap.add_argument("--interval", type=float, default=1.0,
                    help="seconds between requests")
    ap.add_argument("--status", action="store_true", help="only ask for status")
    ap.add_argument("--probe", type=float, metavar="US",
                    help="send probe requests instead of pulses: the gate opens "
                         "US microseconds after a frame timestamp and closes one "
                         "period later")
    ap.add_argument("--timeout", type=float, default=5.0, help="reply timeout, s")
    ap.add_argument("--csv", help="append one row per reply to this file")
    args = ap.parse_args()

    ctx = zmq.Context()
    sock = ctx.socket(zmq.REQ)
    sock.setsockopt(zmq.RCVTIMEO, int(args.timeout * 1000))
    sock.setsockopt(zmq.LINGER, 0)
    sock.connect(args.endpoint)

    if args.status:
        sock.send_string("status")
        print(sock.recv_string())
        return 0

    writer = None
    if args.csv:
        f = open(args.csv, "a", newline="")
        writer = csv.writer(f)
        if f.tell() == 0:
            writer.writerow(["id", "ok", "clean", "ptp_ns", "frame_id",
                             "frame_received", "round_trip_ms", "error"])
    failures = 0
    for i in range(args.count):
        if i:
            time.sleep(args.interval)
        t0 = time.monotonic()
        if args.probe is not None:
            sock.send_string(f"probe {args.probe:g} {i}")
        else:
            sock.send_string(f"pulse {i}")
        try:
            reply = json.loads(sock.recv_string())
        except zmq.Again:
            print(f"pulse {i}: no reply within {args.timeout} s", file=sys.stderr)
            return 1
        rtt_ms = (time.monotonic() - t0) * 1e3
        ok = reply.get("ok", False)
        if args.probe is not None:
            failures += not ok
            frames = reply.get("frame_ptp_ns", [0, 0])
            opened = reply.get("gate_open_ptp_ns", [0, 0])
            closed = reply.get("gate_close_ptp_ns", [0, 0])
            print(f"probe {i}: ok={ok} frames {frames} gate open "
                  f"[{(opened[0] - frames[0]) / 1e3:.0f}, {(opened[1] - frames[0]) / 1e3:.0f}] us "
                  f"after the first, closed [{(closed[0] - frames[1]) / 1e3:.0f}, "
                  f"{(closed[1] - frames[1]) / 1e3:.0f}] us after the second "
                  f"({rtt_ms:.1f} ms){'  ' + reply['error'] if 'error' in reply else ''}")
            continue
        failures += not ok or not reply.get("clean", False)
        print(f"pulse {i}: ok={ok} clean={reply.get('clean')} "
              f"ptp_ns={reply.get('ptp_ns')} frame_id={reply.get('frame_id')} "
              f"({rtt_ms:.1f} ms){'  ' + reply['error'] if 'error' in reply else ''}")
        if writer:
            writer.writerow([reply.get("id"), ok, reply.get("clean"),
                             reply.get("ptp_ns"), reply.get("frame_id"),
                             reply.get("frame_received"), f"{rtt_ms:.3f}",
                             reply.get("error", "")])
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
