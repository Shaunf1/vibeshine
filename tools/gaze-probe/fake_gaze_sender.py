#!/usr/bin/env python3
"""Fake gaze_probe for testing gaze_listener.py without a headset.

Sends the same 48-byte sample packets as gaze_probe at a fixed rate, with the gaze
sweeping in a slow circle, and reads the listener's acks to fill in rtt_us.

    python tools/gaze-probe/fake_gaze_sender.py 127.0.0.1 48200 --duration 5
"""
from __future__ import annotations

import argparse
import math
import select
import socket
import sys
import time

from gaze_listener import ACK, ACK_MAGIC, FLAG_FOCUSED, FLAG_SAMPLE_TIME, FLAG_VALID, SAMPLE, SAMPLE_MAGIC


def build_sample(seq: int, rtt_us: int, t: float, valid: bool, radius_deg: float = 15.0) -> bytes:
    flags = FLAG_FOCUSED
    x = y = z = 0.0
    if valid:
        flags |= FLAG_VALID | FLAG_SAMPLE_TIME
        yaw = math.radians(radius_deg * math.cos(t))
        pitch = math.radians(radius_deg * math.sin(t))
        x = math.sin(yaw) * math.cos(pitch)
        y = math.sin(pitch)
        z = -math.cos(yaw) * math.cos(pitch)
    return SAMPLE.pack(
        SAMPLE_MAGIC, 1, flags, seq & 0xFFFFFFFF, rtt_us, time.time_ns(), time.monotonic_ns(), x, y, z, 0.0
    )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("--rate", type=float, default=90.0)
    ap.add_argument("--duration", type=float, default=5.0)
    ap.add_argument("--drop-every", type=int, default=0, help="skip every Nth seq to simulate loss")
    ap.add_argument("--invalid-every", type=int, default=0, help="mark every Nth sample invalid (blink)")
    args = ap.parse_args(argv)

    sock = socket.socket(socket.AF_INET6 if ":" in args.host else socket.AF_INET, socket.SOCK_DGRAM)
    sock.setblocking(False)
    dest = (args.host, args.port)
    period = 1.0 / args.rate
    start = time.monotonic()
    next_t = start
    seq = sent = acks = 0
    rtt_us = 0
    while time.monotonic() - start < args.duration:
        t = time.monotonic() - start
        if not (args.drop_every and seq % args.drop_every == args.drop_every - 1):
            valid = not (args.invalid_every and seq % args.invalid_every == 0)
            try:
                sock.sendto(build_sample(seq, rtt_us, t, valid), dest)
                sent += 1
            except OSError:
                pass  # e.g. ICMP port unreachable before the listener starts
        seq += 1
        # Wait briefly for the ack so the RTT is not padded by the send period.
        select.select([sock], [], [], period / 2)
        while True:
            try:
                data = sock.recv(64)
            except (BlockingIOError, ConnectionResetError, OSError):
                break
            if len(data) == ACK.size:
                magic, _, echo_ns = ACK.unpack(data)
                if magic == ACK_MAGIC:
                    rtt_us = max(0, (time.time_ns() - echo_ns) // 1000)
                    acks += 1
        next_t += period
        delay = next_t - time.monotonic()
        if delay > 0:
            time.sleep(delay)
    print(f"fake sender: sent={sent} acks={acks} last_rtt_us={rtt_us}")
    return 0 if acks else 1


if __name__ == "__main__":
    sys.exit(main())
