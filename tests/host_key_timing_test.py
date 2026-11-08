"""Selected-key timing capture uses fake clocks/status only; no USB access."""
import unittest

from host.client import DeviceError, KeyboardClient
from host.mcp_diagnostics import capture_key_timing, register_diagnostic_tools
from host.session import KeyboardSession
from tests.host_session_diagnostics_test import Clock, RecordingTransport


class Client:
    def __init__(self, clock, transform=None):
        self.clock, self.transform = clock, transform
        self.transport = type("Transport", (), {"demo": True})()
        self.calls = []
        self.reads = 0
        self.infos = 0
    def info(self):
        self.infos += 1
        self.calls.append((1, None))
        return dict(board_rev=86, generation=1, config_crc=123)
    def status(self, side):
        self.calls.append((2, side))
        n = self.reads
        self.reads += 1
        t = self.clock.now
        down = .04 <= t < .14
        result = dict(fresh=True, sample_sequence=n+1, link_age_us=1000,
                      calibration_state=0, raw=[2000 if down else 1000]*7,
                      travel=[500 if down else 0]*7,
                      pressed_mask=(1 << 9) if down else 0)
        if self.transform:
            result = self.transform(result, n)
        return result


class TimingTests(unittest.TestCase):
    def run_capture(self, transform=None, **kwargs):
        clock = Clock()
        client = Client(clock, transform)
        return capture_key_timing(client, 9, .2, 100, clock=clock.clock, delay=clock.delay, **kwargs), client

    def test_actual_pressed_bit_selected_side_edge_brackets_and_holds(self):
        r, c = self.run_capture()
        self.assertEqual(r['fresh_snapshots'], 21)
        self.assertTrue(all(side == 1 for op, side in c.calls if op == 2))
        self.assertEqual((r['press_count'], r['release_count']), (1, 1))
        self.assertEqual(len(r['complete_holds']), 1)
        press, release = r['edges']
        self.assertAlmostEqual(press['time_ms'], 34)
        self.assertAlmostEqual(press['earliest_ms'], 29)
        self.assertAlmostEqual(press['latest_ms'], 40)
        hold = r['complete_holds'][0]
        self.assertAlmostEqual(hold['duration_ms'], 100)
        self.assertLessEqual(hold['minimum_ms'], 100)
        self.assertGreaterEqual(hold['maximum_ms'], 100)
        self.assertEqual(r['samples'][0]['reported_age_us'], 1000)
        self.assertAlmostEqual(r['samples'][0]['estimated_sample_ms'], -1)
        self.assertFalse(r['output_changed'])
        self.assertFalse(r['configuration_changed'])
        self.assertTrue(r['timing_usable'])

    def test_duplicate_stale_reset_and_gap_do_not_join_edges(self):
        def stale(r, n):
            if n == 3: r['fresh'] = False
            return r
        r, _ = self.run_capture(stale)
        self.assertEqual(r['press_count'], 0)
        self.assertEqual(r['release_count'], 1)
        self.assertFalse(r['complete_holds'])
        self.assertEqual(r['counts']['stale'], 1)
        def reset(r, n):
            r['sample_sequence'] = n+100 if n < 4 else n-3
            return r
        r, _ = self.run_capture(reset)
        self.assertEqual(r['press_count'], 0)
        self.assertEqual(r['counts']['resets'], 1)
        def frozen(r, n):
            r['sample_sequence'] = 1
            return r
        r, _ = self.run_capture(frozen)
        self.assertEqual(r['fresh_snapshots'], 1)
        self.assertEqual(r['counts']['duplicates'], 20)
        self.assertFalse(r['timing_usable'])

    def test_full_sequence_wrap_is_valid(self):
        def wrapping(r, n):
            r['sample_sequence'] = (0xfffffffd + n) & 0xffffffff
            return r
        r, _ = self.run_capture(wrapping)
        self.assertEqual(r['counts']['resets'], 0)
        self.assertEqual(r['press_count'], 1)

    def test_limits_and_invalid_arguments_reject_before_io(self):
        clock = Clock()
        c = Client(clock)
        for kwargs in (dict(key=True), dict(key=14), dict(duration_seconds=.1),
                       dict(duration_seconds=10.1), dict(sample_hz=101), dict(include_samples=1)):
            args = dict(key=0, duration_seconds=.2, sample_hz=100, include_samples=True)
            args.update(kwargs)
            with self.assertRaises(ValueError):
                capture_key_timing(c, clock=clock.clock, delay=clock.delay, **args)
        self.assertFalse(c.calls)
        r = capture_key_timing(c, 0, 10, 100, False, clock=clock.clock, delay=clock.delay)
        self.assertLessEqual(r['fresh_snapshots'], 1001)
        self.assertNotIn('samples', r)

    def test_status_only_does_not_change_live_output_even_readonly(self):
        clock = Clock()
        transport = RecordingTransport()
        transport.enabled = True
        client = KeyboardClient(transport, write_enabled=False)
        capture_key_timing(client, 0, .2, 5, clock=clock.clock, delay=clock.delay)
        self.assertTrue(transport.enabled)
        self.assertTrue(all(op in (1, 2) for op in transport.calls))

    def test_long_gap_and_age_invalidate_transition(self):
        clock = Clock()
        client = Client(clock)
        original_status = client.status
        def delayed(side):
            if client.reads == 3: clock.now += .1
            return original_status(side)
        client.status = delayed
        result = capture_key_timing(client, 9, .2, 100, clock=clock.clock, delay=clock.delay)
        self.assertGreater(result['counts']['gaps'], 0)
        self.assertEqual(result['press_count'], 0)
        r, _ = self.run_capture(lambda r, n: {**r, 'link_age_us': 6000} if n == 3 else r)
        self.assertEqual(r['press_count'], 0)
        self.assertEqual(r['counts']['stale'], 1)

    def test_changed_configuration_marks_measurement_unusable(self):
        clock = Clock()
        client = Client(clock)
        original = client.info
        def info():
            r = original()
            if client.infos > 1: r['config_crc'] = 321
            return r
        client.info = info
        r = capture_key_timing(client, 9, .2, 100, clock=clock.clock, delay=clock.delay)
        self.assertTrue(r['configuration_changed'])
        self.assertFalse(r['timing_usable'])

    def test_active_batch_rejected_before_capture(self):
        class Server:
            def __init__(self): self.tools = {}
            def tool(self, **_):
                def register(fn): self.tools[fn.__name__] = fn; return fn
                return register
        server, session = Server(), KeyboardSession()
        session.calibration = type('Active', (), {'active': True})()
        register_diagnostic_tools(server, session, None, None, lambda fn: fn)
        with self.assertRaisesRegex(DeviceError, 'calibration'):
            server.tools['keyboard_capture_key_timing'](0)


if __name__ == '__main__':
    unittest.main()
