import struct
import unittest
from host.performance import (FIELDS, PERFORMANCE, LINK_FIELDS, LINK_PERFORMANCE,
    decode_performance, decode_link_performance, compare_samples, measure, measure_link)
from host.client import KeyboardClient
from host.protocol import packet, unpack_packet


class PerformanceTests(unittest.TestCase):
    def samples(self):
        a = dict.fromkeys(FIELDS, 0)
        a.update(micros=0xFFFF0000, usb_completed=0xFFFFF000, adc_sequence=0xFFFF0000, scan_target_hz=64000)
        b = dict(a)
        b.update(micros=(a["micros"]+5_000_000)&0xFFFFFFFF, usb_completed=(a["usb_completed"]+40000)&0xFFFFFFFF, adc_sequence=(a["adc_sequence"]+320000)&0xFFFFFFFF, adc_min_period_cycles=3750, adc_max_period_cycles=3900, adc_irq_max_cycles=960)
        return a, b
    def test_wrapped_counters_and_time(self):
        result = compare_samples(*self.samples(), 5)
        self.assertEqual(result["usb_completion_hz"], 8000)
        self.assertEqual(result["adc_scan_hz"], 64000)
        self.assertEqual(result["adc_lifetime_min_period_us"], 15.625)
        self.assertEqual(result["adc_lifetime_max_irq_us"], 4)
    def test_readonly_opcode(self):
        samples = list(self.samples())
        class Transport:
            def write(self, raw):
                op, seq, status, data = unpack_packet(raw)
                assert (op, status, data) == (PERFORMANCE, 0, b"")
                values = samples.pop(0)
                self.reply = packet(op|128, seq, struct.pack("<13I", *(values[name] for name in FIELDS)))
            def read(self, timeout):
                return self.reply
        ticks = iter([1, 6])
        self.assertEqual(measure(Transport(), delay=lambda _: None, clock=lambda: next(ticks))["usb_completion_hz"], 8000)
    def test_reset_or_schema_rejected(self):
        a, b = self.samples()
        b["micros"] = 200000
        with self.assertRaises(ValueError):
            compare_samples(a, b, 5)
        with self.assertRaises(ValueError):
            decode_performance(b"\0"*51)

    def link_samples(self):
        first = {name: 0xFFFFF000 for name in LINK_FIELDS}
        last = {name: (value + 40000) & 0xFFFFFFFF for name, value in first.items()}
        last.update(micros=(first["micros"] + 5_000_000) & 0xFFFFFFFF,
                    accepted=(first["accepted"] + 39500) & 0xFFFFFFFF,
                    rtt_min_us=12, rtt_max_us=108)
        return first, last

    def link_transport(self, samples):
        samples = list(samples)
        class Transport:
            def __init__(self): self.calls = []
            def write(self, raw):
                op, seq, status, data = unpack_packet(raw)
                self.calls.append((op, data))
                assert (op, status, data) == (LINK_PERFORMANCE, 0, b"")
                values = samples.pop(0)
                self.reply = packet(op | 128, seq, struct.pack("<13I", *(values[name] for name in LINK_FIELDS)))
            def read(self, timeout): return self.reply
        return Transport()

    def test_link_wrapped_counters_order_lifetime_rtt_and_readonly(self):
        first, last = self.link_samples()
        t = self.link_transport((first, last))
        ticks = iter((1, 6))
        result = measure_link(t, delay=lambda _: None, clock=lambda: next(ticks))
        self.assertEqual(result["poll_hz"], 8000)
        self.assertEqual(result["accepted_hz"], 7900)
        self.assertEqual(result["deltas"]["framing_errors"], 40000)
        self.assertEqual((result["lifetime_rtt_min_us"], result["lifetime_rtt_max_us"]), (12, 108))
        self.assertEqual(result["last"], last)
        self.assertEqual(t.calls, [(0x41, b""), (0x41, b"")])
        ordered = decode_link_performance(struct.pack("<13I", *range(13)))
        self.assertEqual(list(ordered.items()), list(zip(LINK_FIELDS, range(13))))

    def test_link_bad_schema_duration_reset_and_zero_time(self):
        for n in (0, 51, 53):
            with self.assertRaises(ValueError): decode_link_performance(bytes(n))
        for seconds in (-1, 0, 0.1, 61, float("nan"), float("inf"), "5"):
            t = self.link_transport(())
            with self.assertRaises(ValueError): measure_link(t, seconds)
            self.assertEqual(t.calls, [])
        first, last = self.link_samples()
        for device_micros, host_seconds in ((first["micros"], 0), (first["micros"], 5), (200000, 5)):
            last["micros"] = device_micros
            ticks = iter((0, host_seconds))
            with self.assertRaisesRegex(ValueError, "reset/disconnection"):
                measure_link(self.link_transport((first, last)), delay=lambda _: None, clock=lambda: next(ticks))

    def test_diagnostics_do_not_identify_or_disable_output(self):
        for op in (PERFORMANCE, LINK_PERFORMANCE):
            class Transport:
                def write(self, raw):
                    command, seq, status, data = unpack_packet(raw)
                    self.command = command
                    assert (command, status, data) == (op, 0, b"")
                    self.reply = packet(op | 128, seq, bytes(52))
                def read(self, timeout): return self.reply
            t = Transport()
            client = KeyboardClient(t, write_enabled=False)
            client.output_enabled = True
            self.assertEqual(client.request(op), bytes(52))
            self.assertEqual(t.command, op)
            self.assertTrue(client.output_enabled)
            self.assertFalse(client.identified)


if __name__ == "__main__":
    unittest.main()
