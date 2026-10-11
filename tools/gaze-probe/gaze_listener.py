#!/usr/bin/env python3
"""PC-side listener for the Steam Frame gaze probe.

Receives gaze sample packets from gaze_probe (see gaze_packet.h), acks each one so the
probe can measure round-trip time, and prints samples, packets per second, loss and a
latency estimate. Standard library only.

    python tools/gaze-probe/gaze_listener.py --port 48200
"""
from __future__ import annotations

import argparse
import math
import socket
import struct
import sys
import time
from dataclasses import dataclass, field

SAMPLE = struct.Struct("<4sHHIIQq4f")  # 48 bytes, matches gaze_sample_packet_t
ACK = struct.Struct("<4sIQ")  # 16 bytes, matches gaze_ack_packet_t
SAMPLE_MAGIC = b"GZP1"
ACK_MAGIC = b"GZA1"
FLAG_VALID = 0x1
FLAG_FOCUSED = 0x2
FLAG_SAMPLE_TIME = 0x4


@dataclass
class Sample:
    flags: int
    seq: int
    rtt_us: int
    send_time_ns: int
    xr_sample_time: int
    dir: tuple[float, float, float]

    @property
    def valid(self) -> bool:
        return bool(self.flags & FLAG_VALID)

    @property
    def focused(self) -> bool:
        return bool(self.flags & FLAG_FOCUSED)

    def yaw_pitch_deg(self) -> tuple[float, float]:
        x, y, z = self.dir
        return math.degrees(math.atan2(x, -z)), math.degrees(math.asin(max(-1.0, min(1.0, y))))


def parse_sample(data: bytes) -> Sample | None:
    if len(data) != SAMPLE.size:
        return None
    magic, version, flags, seq, rtt_us, send_ns, xr_time, x, y, z, _ = SAMPLE.unpack(data)
    if magic != SAMPLE_MAGIC or version != 1:
        return None
    return Sample(flags, seq, rtt_us, send_ns, xr_time, (x, y, z))


def make_ack(sample: Sample) -> bytes:
    return ACK.pack(ACK_MAGIC, sample.seq, sample.send_time_ns)


@dataclass
class Window:
    """Counters for one reporting window."""

    packets: int = 0
    valid: int = 0
    focused: int = 0
    lost: int = 0
    reordered: int = 0
    one_way_ms: list[float] = field(default_factory=list)
    rtts_us: list[int] = field(default_factory=list)


class Stats:
    """Tracks loss, rate and latency across packets."""

    def __init__(self) -> None:
        self.window = Window()
        self.window_start = time.monotonic()
        self.last_seq: int | None = None
        self.total = 0
        self.min_offset_ms: float | None = None

    def add(self, s: Sample, recv_ns: int) -> None:
        w = self.window
        w.packets += 1
        self.total += 1
        w.valid += s.valid
        w.focused += s.focused
        if s.rtt_us:
            w.rtts_us.append(s.rtt_us)
        if self.last_seq is not None:
            gap = (s.seq - self.last_seq) & 0xFFFFFFFF
            if gap == 0 or gap > 0x7FFFFFFF:
                w.reordered += 1
                return
            w.lost += gap - 1
        self.last_seq = s.seq
        # Wall-clock one-way delay. Only meaningful when both clocks are NTP-synced;
        # the spread (jitter) is meaningful either way.
        offset = (recv_ns - s.send_time_ns) / 1e6
        w.one_way_ms.append(offset)
        if self.min_offset_ms is None or offset < self.min_offset_ms:
            self.min_offset_ms = offset

    def due(self, now: float, interval: float) -> bool:
        return now - self.window_start >= interval

    def report(self, now: float) -> str:
        w = self.window
        dt = max(now - self.window_start, 1e-6)
        expected = w.packets + w.lost
        loss = 100.0 * w.lost / expected if expected else 0.0
        parts = [
            f"STATS pps={w.packets / dt:6.1f}",
            f"valid={100.0 * w.valid / w.packets if w.packets else 0:5.1f}%",
            f"focused={100.0 * w.focused / w.packets if w.packets else 0:5.1f}%",
            f"lost={w.lost} ({loss:.1f}%)",
        ]
        if w.reordered:
            parts.append(f"reordered={w.reordered}")
        if w.rtts_us:
            rtts = sorted(w.rtts_us)
            parts.append(f"latency~{rtts[len(rtts) // 2] / 2000.0:.2f}ms (median rtt/2, min {rtts[0] / 2000.0:.2f}ms)")
        else:
            parts.append("latency=? (no rtt yet: are acks reaching the probe?)")
        if w.one_way_ms:
            ow = sorted(w.one_way_ms)
            p50 = ow[len(ow) // 2]
            jitter = ow[int(len(ow) * 0.95)] - ow[0]
            parts.append(f"wall_one_way_p50={p50:.2f}ms jitter95={jitter:.2f}ms")
        self.window = Window()
        self.window_start = now
        return " ".join(parts)


def format_sample(s: Sample, recv_ns: int) -> str:
    yaw, pitch = s.yaw_pitch_deg() if s.valid else (0.0, 0.0)
    x, y, z = s.dir
    return (
        f"GAZE seq={s.seq} valid={int(s.valid)} focused={int(s.focused)} "
        f"dir=({x:+.3f} {y:+.3f} {z:+.3f}) yaw={yaw:+6.1f} pitch={pitch:+6.1f} "
        f"rtt_us={s.rtt_us} wall_delay_ms={(recv_ns - s.send_time_ns) / 1e6:.2f}"
    )


def serve(sock: socket.socket, args: argparse.Namespace, out=sys.stdout) -> int:
    """Run the receive loop. Returns the number of sample packets received."""
    stats = Stats()
    sock.settimeout(0.2)
    deadline = time.monotonic() + args.duration if args.duration else None
    sample_every = max(1, args.print_every)
    peer = None
    while deadline is None or time.monotonic() < deadline:
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            data = None
        now = time.monotonic()
        if data is not None:
            recv_ns = time.time_ns()
            s = parse_sample(data)
            if s is None:
                print(f"IGNORED {len(data)} bytes from {addr[0]}:{addr[1]}", file=out)
            else:
                if args.allow and addr[0] != args.allow:
                    continue
                if peer != addr:
                    peer = addr
                    print(f"PEER {addr[0]}:{addr[1]}", file=out)
                sock.sendto(make_ack(s), addr)
                stats.add(s, recv_ns)
                if not args.quiet and s.seq % sample_every == 0:
                    print(format_sample(s, recv_ns), file=out)
        if stats.due(now, args.interval):
            if stats.window.packets or stats.total:
                print(stats.report(now), file=out)
            else:
                stats.window_start = now
                print("WAITING no packets yet", file=out)
            out.flush()
    return stats.total


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bind", default="0.0.0.0", help="address to listen on (default 0.0.0.0)")
    ap.add_argument("--port", type=int, default=48200, help="UDP port (default 48200)")
    ap.add_argument("--allow", help="only accept packets from this IP (the Frame)")
    ap.add_argument("--interval", type=float, default=1.0, help="seconds between STATS lines")
    ap.add_argument("--print-every", type=int, default=9, help="print every Nth sample (default 9, ~10 lines/s at 90 Hz)")
    ap.add_argument("--quiet", action="store_true", help="print STATS lines only")
    ap.add_argument("--duration", type=float, default=0.0, help="exit after this many seconds")
    args = ap.parse_args(argv)

    family = socket.AF_INET6 if ":" in args.bind else socket.AF_INET
    sock = socket.socket(family, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    print(f"Listening for gaze samples on {args.bind}:{args.port} (Ctrl+C to stop)")
    try:
        total = serve(sock, args)
    except KeyboardInterrupt:
        return 0
    finally:
        sock.close()
    return 0 if total else 1


if __name__ == "__main__":
    sys.exit(main())
