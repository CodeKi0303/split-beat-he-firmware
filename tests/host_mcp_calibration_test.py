"""Offline sensor sequence/freshness/partial-write tests; no USB enumeration."""
import copy
import unittest

from host.client import KeyboardClient, DeviceError
from host.mcp_calibration import CaptureModel, apply_capture
from host.simulator import DemoTransport, defaults


def status(seq, value=1000):
    return dict(fresh=True, calibration_state=0, output_enabled=False,
                sample_sequence=seq, raw=[value]*7)


def baseline():
    m = CaptureModel(defaults(), 0)
    for n in range(151):
        for side in (0, 1):
            m.add(side, status(n), n*.02)
    return m


class CalibrationTests(unittest.TestCase):
    def test_stable_baseline_motion_confirmation_and_preservation(self):
        m = baseline()
        self.assertEqual(m.state, "press")
        m.add(0, status(151, 1020), 3.02)
        self.assertFalse(m.snapshot(3.02)["keys"][0]["detected"])
        m.add(0, status(152, 1020), 3.04)
        self.assertTrue(m.snapshot(3.04)["keys"][0]["detected"])
        self.assertFalse(m.candidates())
        m.add(0, status(153, 2000), 3.06)
        m.add(0, status(154, 2000), 3.08)
        self.assertEqual(len(m.candidates()), 7)
        m.state = "ready"
        c = KeyboardClient(DemoTransport(), write_enabled=True)
        r = apply_capture(c, m, [1, 2])
        self.assertTrue(r["applied_ram"])
        p = c.get_config()
        self.assertEqual(p["keys"][0], m.original["keys"][0])
        self.assertEqual(p["bindings"], m.original["bindings"])
        self.assertEqual(p["keys"][1]["pressed"], 2000)
        self.assertEqual(p["keys"][1]["rt_press"], 100)
        self.assertFalse(c.transport.stored)
        self.assertFalse(c.transport.enabled)

    def test_duplicate_sample_does_not_refresh_stream(self):
        m = CaptureModel(defaults(), 0)
        m.add(0, status(1), 0)
        m.add(1, status(1), 0)
        m.add(0, status(1), .49)
        with self.assertRaisesRegex(ValueError, "stopped"):
            m.add(0, status(1), .51)

    def test_sequence_rollback_stale_output_and_bad_adc_rejected(self):
        for patch in ({"fresh": False}, {"output_enabled": True}, {"calibration_state": 1}, {"raw": [4096]*7}, {"sample_sequence": True}):
            with self.subTest(patch=patch):
                m = CaptureModel(defaults(), 0)
                with self.assertRaises(ValueError):
                    m.add(0, {**status(1), **patch}, 0)
        m = CaptureModel(defaults(), 0)
        m.add(0, status(8), 0)
        with self.assertRaisesRegex(ValueError, "backwards"):
            m.add(0, status(7), .02)

    def test_unstable_baseline_and_short_spike_rejected(self):
        m = CaptureModel(defaults(), 0)
        with self.assertRaisesRegex(ValueError, "unstable"):
            for n in range(151):
                for side in (0, 1):
                    m.add(side, status(n, 1000+(100 if n == 50 else 0)), n*.02)
        m = baseline()
        m.add(0, status(151, 2000), 3.02)
        m.add(0, status(152, 1000), 3.04)
        self.assertFalse(m.candidates())

    def test_external_configuration_change_blocks_apply(self):
        m = baseline()
        m.add(0, status(151, 2000), 3.02)
        m.add(0, status(152, 2000), 3.04)
        m.state = "ready"
        c = KeyboardClient(DemoTransport(), write_enabled=True)
        c.transport.profile["keys"][5]["actuation"] = 300
        with self.assertRaisesRegex(DeviceError, "changed"):
            apply_capture(c, m)
        self.assertFalse(c.transport.dirty)

    def test_lost_ack_readback_and_partial_write(self):
        for write_first in (True, False):
            m = baseline()
            m.add(0, status(151, 2000), 3.02)
            m.add(0, status(152, 2000), 3.04)
            m.state = "ready"
            c = KeyboardClient(DemoTransport(), write_enabled=True)
            setter = c.set_key
            def fail(record):
                if write_first:
                    setter(record)
                raise TimeoutError("lost ACK")
            c.set_key = fail
            r = apply_capture(c, m, [0])
            self.assertEqual(r["applied_ram"], write_first)
            self.assertTrue(r["verified"])
            self.assertFalse(r["persisted"])
    def test_external_change_after_output_blocks_calibration_writes(self):
        m = baseline()
        m.add(0, status(151, 2000), 3.02)
        m.add(0, status(152, 2000), 3.04)
        m.state = "ready"
        c = KeyboardClient(DemoTransport(), write_enabled=True)
        output = c.output
        def changed(enabled):
            result = output(enabled)
            c.transport.profile["keys"][5]["actuation"] = 300
            return result
        c.output = changed
        with self.assertRaisesRegex(DeviceError, "after output"):
            apply_capture(c, m)
        self.assertTrue(all(not key["flags"] & 4 for key in c.get_config()["keys"]))


if __name__ == "__main__":
    unittest.main()
