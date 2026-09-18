import math
import unittest

from analysis import bandwidth as bw


class BandwidthTest(unittest.TestCase):
    def test_payload_per_packet(self):
        self.assertEqual(bw.payload_per_packet(1500), 1408)
        self.assertEqual(bw.payload_per_packet(4200), 4096)
        self.assertEqual(bw.payload_per_packet(9000), 8832)

    def test_link_payload(self):
        self.assertAlmostEqual(bw.link_payload_gbps(10.0, 1500), 9.37, places=2)
        self.assertAlmostEqual(bw.link_payload_gbps(1.0, 1500), 0.937, places=3)
        self.assertGreater(bw.link_payload_gbps(10.0, 4200), 9.7)

    def test_stream_rates_from_design_matrix(self):
        # Row A1: one IMX676 full-res RAW10 at 40 fps.
        self.assertAlmostEqual(bw.stream_gbps(3552, 3552, 10, 40), 5.047, places=3)
        # Row C1: four binned RAW12 streams at 60 fps saturate 10G.
        self.assertAlmostEqual(4 * bw.stream_gbps(1768, 1768, 12, 60), 9.002, places=3)
        # Row D1: four binned RAW10 streams at 7 fps fit a 1G link.
        self.assertLess(4 * bw.stream_gbps(1768, 1768, 10, 7), bw.link_payload_gbps(1.0, 1500))

    def test_packets_per_second(self):
        pps = bw.packets_per_second(3552, 3552, 10, 36, 1500)
        self.assertTrue(400_000 < pps < 410_000, pps)

    def test_max_fps(self):
        fps = bw.max_fps(3552, 3552, 10, 5.5)
        self.assertTrue(math.isclose(fps, 43.6, rel_tol=0.01), fps)


if __name__ == "__main__":
    unittest.main()
