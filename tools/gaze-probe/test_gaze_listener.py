"""Tests gaze_listener.py against fake_gaze_sender.py over loopback UDP.

    python -m unittest discover -s tools/gaze-probe -p "test_*.py" -v
"""
from __future__ import annotations

import argparse
import io
import re
import socket
import threading
import unittest

import fake_gaze_sender
import gaze_listener


class PacketTests(unittest.TestCase):
    def test_sizes_match_c_header(self) -> None:
        self.assertEqual(gaze_listener.SAMPLE.size, 48)
        self.assertEqual(gaze_listener.ACK.size, 16)

    def test_round_trip(self) -> None:
        s = gaze_listener.parse_sample(fake_gaze_sender.build_sample(7, 1234, 0.0, True))
        self.assertIsNotNone(s)
        self.assertEqual(s.seq, 7)
        self.assertEqual(s.rtt_us, 1234)
        self.assertTrue(s.valid and s.focused)
        x, y, z = s.dir
        self.assertAlmostEqual(x * x + y * y + z * z, 1.0, places=5)
        yaw, pitch = s.yaw_pitch_deg()
        self.assertAlmostEqual(yaw, 15.0, places=3)
        self.assertAlmostEqual(pitch, 0.0, places=3)

    def test_rejects_garbage(self) -> None:
        self.assertIsNone(gaze_listener.parse_sample(b"hello"))
        self.assertIsNone(gaze_listener.parse_sample(b"XXXX" + bytes(44)))

    def test_loss_counting(self) -> None:
        stats = gaze_listener.Stats()
        for seq in (0, 1, 2, 5, 6):
            raw = fake_gaze_sender.build_sample(seq, 0, 0.0, True)
            stats.add(gaze_listener.parse_sample(raw), 0)
        self.assertEqual(stats.window.lost, 2)
        self.assertEqual(stats.window.packets, 5)


class LoopbackTest(unittest.TestCase):
    def test_listener_with_fake_sender(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        out = io.StringIO()
        args = argparse.Namespace(duration=3.0, interval=1.0, print_every=9, quiet=False, allow=None)
        result = {}
        t = threading.Thread(target=lambda: result.setdefault("total", gaze_listener.serve(sock, args, out)))
        t.start()
        rc = fake_gaze_sender.main(["127.0.0.1", str(port), "--duration", "2.5", "--drop-every", "30", "--invalid-every", "10"])
        t.join()
        sock.close()
        text = out.getvalue()
        print(text)
        self.assertEqual(rc, 0, "fake sender received no acks")
        self.assertGreater(result["total"], 150)
        self.assertIn("GAZE seq=", text)
        rates = [float(m) for m in re.findall(r"pps=\s*([\d.]+)", text)]
        self.assertTrue(any(70 <= r <= 95 for r in rates), rates)
        self.assertRegex(text, r"lost=[1-9]")
        self.assertRegex(text, r"latency~[\d.]+ms")


if __name__ == "__main__":
    unittest.main()
