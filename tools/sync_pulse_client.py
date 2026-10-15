#!/usr/bin/env python3
"""Ask orange for sync pulses over ZeroMQ, the way the Bonsai PC does.

Each request makes orange let one exposure of the sync camera through to its
GPO line and reply with that frame's PTP timestamp (see docs/zmq_sync.md).

    uv run --with pyzmq tools/sync_pulse_client.py tcp://<orange-pc>:5560 \
        --count 10 --interval 2 --csv pulses.csv
    uv run --with pyzmq tools/sync_pulse_client.py tcp://<orange-pc>:5560 --status
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
        sock.send_string(f"pulse {i}")
        try:
            reply = json.loads(sock.recv_string())
        except zmq.Again:
            print(f"pulse {i}: no reply within {args.timeout} s", file=sys.stderr)
            return 1
        rtt_ms = (time.monotonic() - t0) * 1e3
        ok = reply.get("ok", False)
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
