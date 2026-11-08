"""Offline MCP lifecycle/diagnostics tests. Fake transports never import HID."""
import struct
import threading
import unittest
from unittest.mock import patch

from host.client import DeviceError, KeyboardClient
from host.session import KeyboardSession
from host.simulator import DemoTransport
from host.protocol import packet, unpack_packet
from host.mcp_diagnostics import (capture_sensors, measure_counters, capability_summary,
                                  read_adc_diagnostics, read_scheduler_diagnostics, read_adc_trace, read_cpu_profile,
                                  read_adc_irq_profile,
                                  register_diagnostic_tools)
from host.performance import (FIELDS, LINK_FIELDS, ADC_DIAGNOSTICS, ADC_DIAGNOSTIC_FIELDS,
                              decode_adc_diagnostics, SCHEDULER_DIAGNOSTICS, SCHEDULER_FIELDS,
                              decode_scheduler_diagnostics, ADC_TRACE, encode_adc_trace_keys,
                              decode_adc_trace, CPU_PROFILE, CPU_PROFILE_FIELDS, CPU_PROFILE_STAGES,
                              decode_cpu_profile_begin, decode_cpu_profile_page,
                              ADC_IRQ_PROFILE, ADC_IRQ_PROFILE_FIELDS, ADC_IRQ_PROFILE_STAGES,
                              decode_adc_irq_profile_begin, decode_adc_irq_profile_page)


class RecordingTransport(DemoTransport):
    def __init__(self, serial="TEST-EXACT-SERIAL"):
        super().__init__()
        self.demo = False
        self.serial = serial
        self.calls = []
        self.closed = False

    def write(self, packet):
        self.calls.append(unpack_packet(packet)[0])
        super().write(packet)

    def close(self):
        self.closed = True
        super().close()


class Clock:
    def __init__(self): self.now = 0.0
    def clock(self): return self.now
    def delay(self, seconds): self.now += seconds


class SensorClient:
    def __init__(self, clock):
        self.transport = type("Transport", (), {"demo": True})()
        self.clock = clock
        self.calls = []
        self.fresh = True
        self.freeze = False
    def status(self, side):
        self.calls.append((2, side))
        t = self.clock.clock()
        return {"fresh": self.fresh,
                "sample_sequence": 1 if self.freeze else round(t * 64000) + 1,
                "raw": [1000 + i + side * 7 + round(t * 100) for i in range(7)],
                "travel": [0] * 7}


class CounterClient:
    def __init__(self, clock, capabilities=0x1ff):
        self.transport = type("Transport", (), {"demo": True})()
        self.clock = clock
        self.capabilities = capabilities
        self.calls = []
        self.sequence = 40
    def info(self):
        self.calls.append(1)
        return {"major": 0, "minor": 3, "patch": 3, "capabilities": self.capabilities}
    def request(self, op):
        self.calls.append(op)
        self.sequence += 1
        t = self.clock.clock()
        names = LINK_FIELDS if op == 0x41 else FIELDS
        values = dict.fromkeys(names, 0)
        values.update(micros=(0xffff0000+round(t*1e6))&0xffffffff)
        if op == 0x41:
            values.update(polls=round(t*8000), accepted=round(t*8000), rtt_min_us=93, rtt_max_us=103)
        else:
            values.update(usb_completed=round(t*8000), adc_sequence=round(t*64000), scan_target_hz=64000)
        return struct.pack("<13I", *(values[name] for name in names))


class ADCDiagnosticClient:
    def __init__(self, *, demo=False, **info):
        self.transport = type("Transport", (), {"demo": demo})()
        self.identity = dict(major=0, minor=3, patch=8, capabilities=0x80,
                             board_rev=86, role=0, demo=demo)
        self.identity.update(info)
        self.calls = []
        self.lock = threading.RLock()
        self.values = dict(zip(ADC_DIAGNOSTIC_FIELDS,
                               (0, 0, 1, 0, 0, 1, 0, 3, 0xfffffff0, 0x30, 16, 12, 96000)))
        self.payload = struct.pack("<13I", *(self.values[name] for name in ADC_DIAGNOSTIC_FIELDS))

    def info(self):
        self.calls.append((1, b""))
        return dict(self.identity)

    def request(self, opcode, payload=b""):
        self.calls.append((opcode, payload))
        return self.payload


def trace_payload(keys=(5, 6)):
    # Nonconstant history, endian boundaries, pressed bit and maximum valid travel.
    return (struct.pack("<IIBB", 0xfffffffe, 0x12345678, *keys)
            + struct.pack("<11H", *range(1000, 1008), 1004, 1005, 0x8000 | 321)
            + struct.pack("<11H", *([4095] * 8), 4095, 4094, 1000))


def cpu_profile_pages(run_id=0xfffffffe):
    pages = [struct.pack("<8I", run_id, 60000240, 240000000, 60000000, 5000, 2000, 12000000, 5001)]
    for page in range(1, 5):
        ids = range((page - 1) * 3, page * 3)
        pages.append(struct.pack("<IB", run_id, 3) + b"".join(
            struct.pack("<B3I", stage, 10 + stage, 2400 * (stage + 1) if stage != 11 else 0,
                        240 * (stage + 1) if stage != 11 else 0) for stage in ids))
    return pages


class CPUProfileClient(ADCDiagnosticClient):
    def __init__(self, **kwargs):
        super().__init__(capabilities=0x800, **kwargs)
        self.pages = cpu_profile_pages()
        self.begin = struct.pack("<I", 0xfffffffe)
        self.fail_at = None

    def request(self, opcode, payload=b""):
        self.calls.append((opcode, payload))
        if payload == self.fail_at:
            raise DeviceError("BUSY")
        assert opcode == CPU_PROFILE
        if payload == b"\x00": return self.begin
        assert len(payload) == 2 and payload[0] == 1
        return self.pages[payload[1]]


def adc_irq_profile_pages(run_id=0xfffffff0, samples=500):
    pages = [struct.pack("<10I", run_id, 120000240, 240000000, 120000000,
                         samples, 2, 10000, 4000, 32000, 0)]
    totals = (1800000, 120000, 240000, 360000, 480000, 600000)
    maxima = (9000, 1000, 2000, 3000, 4000, 5000)
    for page in range(1, 3):
        pages.append(struct.pack("<IB", run_id, 3) + b"".join(
            struct.pack("<B3I", stage, samples, totals[stage] if samples else 0,
                        maxima[stage] if samples else 0) for stage in range((page - 1) * 3, page * 3)))
    return pages


class ADCIRQProfileClient(ADCDiagnosticClient):
    def __init__(self, **kwargs):
        super().__init__(capabilities=0x1000, **kwargs)
        self.pages = adc_irq_profile_pages()
        self.begin = struct.pack("<I", 0xfffffff0)
        self.fail_at = None

    def request(self, opcode, payload=b""):
        self.calls.append((opcode, payload))
        if payload == self.fail_at: raise DeviceError("BUSY")
        assert opcode == ADC_IRQ_PROFILE
        if payload == b"\x00": return self.begin
        assert len(payload) == 2 and payload[0] == 1
        return self.pages[payload[1]]


class SessionTests(unittest.TestCase):
    def test_lazy_startup_remembers_serial_and_requires_explicit_connection(self):
        with patch("host.session.HidTransport") as hid:
            session = KeyboardSession(serial="ABC", write_enabled=True)
            hid.assert_not_called()
            self.assertFalse(session.status()["connected"])
            self.assertEqual(session.status()["preferred_serial"], "ABC")
            with self.assertRaises(DeviceError): session.require_client()
        with self.assertRaises(ValueError): KeyboardSession().connect()

    def test_connect_reads_identity_only_disconnect_never_changes_output(self):
        transport = RecordingTransport("ABC")
        with patch("host.session.HidTransport", return_value=transport) as hid:
            session = KeyboardSession(serial="ABC", write_enabled=True)
            result = session.connect()
            hid.assert_called_once_with("ABC")
            self.assertEqual(transport.calls, [1])
            self.assertFalse(result["changed_device_settings"])
            self.assertEqual(result["serial"], "ABC")
            session.travel_mm[0] = 3.2
            with self.assertRaises(DeviceError): session.connect("OTHER")
            session.disconnect()
            self.assertEqual(transport.calls, [1])
            self.assertTrue(transport.closed)
        next_transport = RecordingTransport("ABC")
        with patch("host.session.HidTransport", return_value=next_transport):
            session.connect()
            self.assertEqual(session.travel_mm[0], 3.2)
            session.disconnect()

    def test_demo_and_provided_client_permissions(self):
        with patch("host.session.HidTransport") as hid:
            session = KeyboardSession(demo=True, write_enabled=True)
            self.assertTrue(session.status()["demo"])
            self.assertTrue(session.require_client().write_enabled)
            hid.assert_not_called()
            session.disconnect()
        client = KeyboardClient(DemoTransport(), write_enabled=True)
        self.assertTrue(KeyboardSession(client=client).write_enabled)

    def test_active_worker_closed_before_transport_release(self):
        order = []
        class Calibration:
            active = True
            def close(self): order.append("worker"); self.active = False
        transport = DemoTransport()
        transport.close = lambda: order.append("transport")
        session = KeyboardSession(client=KeyboardClient(transport))
        session.calibration = Calibration()
        session.interference = {"old": True}
        with self.assertRaises(DeviceError): session.require_idle()
        session.disconnect()
        self.assertEqual(order, ["worker", "transport"])
        self.assertIsNone(session.calibration)
        self.assertIsNone(session.interference)


class DiagnosticsTests(unittest.TestCase):
    def diagnostic_tools(self):
        class Server:
            def __init__(self): self.tools = {}
            def tool(self, **kwargs):
                def register(fn): self.tools[fn.__name__] = fn; return fn
                return register
        server = Server()
        session = KeyboardSession(demo=True)
        register_diagnostic_tools(server, session, None, None, lambda fn: fn)
        self.addCleanup(session.disconnect)
        return session, server.tools

    @staticmethod
    def stats(selected_change=0, other_change=0, movement=0):
        return {"statistics": [{"key":i, "mean":1000 + (selected_change if i == 0 else other_change),
                                 "stdev":1, "peak_to_peak":movement} for i in range(14)]}

    def test_bounded_snapshot_statistics_and_readonly_commands(self):
        clock = Clock()
        client = SensorClient(clock)
        result = capture_sensors(client, .2, 10, True, clock=clock.clock, delay=clock.delay)
        self.assertEqual(result["fresh_snapshots_per_side"], [3, 3])
        self.assertEqual(len(result["samples"]), 3)
        self.assertEqual(result["statistics"][0]["mean"], 1010)
        self.assertAlmostEqual(result["statistics"][0]["variance"], 200/3)
        self.assertEqual(result["statistics"][13]["min"], 1013)
        self.assertEqual(result["statistics"][13]["max"], 1033)
        self.assertTrue(all(op == 2 for op, _ in client.calls))
        self.assertTrue(result["demo"])

    def test_invalid_or_stale_capture_never_returns_false_measurement(self):
        clock, client = Clock(), None
        client = SensorClient(clock)
        for seconds in (False, -1, 0, .1, 10.01, float("nan"), float("inf"), "1"):
            with self.assertRaises(ValueError): capture_sensors(client, seconds)
        self.assertEqual(client.calls, [])
        client.fresh = False
        with self.assertRaisesRegex(DeviceError, "stale"):
            capture_sensors(client, .2, 5, clock=clock.clock, delay=clock.delay)
        client.fresh, client.freeze = True, True
        with self.assertRaisesRegex(DeviceError, "distinct"):
            capture_sensors(client, .2, 5, clock=clock.clock, delay=clock.delay)

    def test_counters_keep_same_client_sequence_and_never_send_mutations(self):
        for link in (False, True):
            clock = Clock()
            client = CounterClient(clock)
            result = measure_counters(client, 2, link=link, clock=clock.clock, delay=clock.delay)
            self.assertEqual(client.sequence, 42)
            self.assertEqual(client.calls, [1, 0x41 if link else 0x40, 0x41 if link else 0x40])
            self.assertEqual(result["accepted_hz" if link else "usb_completion_hz"], 8000)
            if not link: self.assertEqual(result["adc_scan_hz"], 64000)

    def test_counter_capability_preflight_and_version_gate(self):
        clock = Clock()
        client = CounterClient(clock, 0x13f)
        result = measure_counters(client)
        self.assertFalse(result["supported"])
        self.assertEqual(client.calls, [1])
        self.assertFalse(capability_summary({"major":0,"minor":3,"patch":1,"capabilities":0x80})["link_performance_counters"])

    def test_fault_before_measurement_is_not_hidden_by_zero_error_delta(self):
        class StoppedADC(CounterClient):
            def request(self, op):
                fields = list(struct.unpack("<13I", super().request(op)))
                fields[5] = 0  # No completed ADC frame.
                fields[6] = fields[8] = 1  # Error/late IRQ happened before window.
                return struct.pack("<13I", *fields)
        clock = Clock()
        result = measure_counters(StoppedADC(clock), 2,
                                  clock=clock.clock, delay=clock.delay)
        self.assertEqual(result["adc_scan_hz"], 0)
        self.assertEqual(result["counter_deltas"]["adc_errors"], 0)
        self.assertEqual(result["counters_before"]["adc_errors"], 1)
        self.assertEqual(result["counters_after"]["adc_late_irqs"], 1)

    def test_adc_diagnostics_decode_preserves_latched_evidence(self):
        client = ADCDiagnosticClient()
        result = read_adc_diagnostics(client)
        self.assertTrue(result["supported"])
        self.assertFalse(result["demo"])
        for name, value in client.values.items():
            self.assertEqual(result[name], value)
        self.assertEqual(result["stop_reason_name"], "OWNERSHIP")
        self.assertEqual(result["stop_phase_cycles"], 0xfffffff0)
        self.assertEqual(client.calls, [(1, b""), (ADC_DIAGNOSTICS, b"")])
        for name in ("changed_device_settings", "changed_output", "changed_firmware"):
            self.assertFalse(result[name])
        self.assertIn("not an absolute latency", result["limits"])

    def test_adc_diagnostics_gate_prevents_unsupported_commands(self):
        for overrides in ({"patch": 7}, {"minor": 2, "patch": 99},
                          {"board_rev": 77}, {"board_rev": 85}, {"role": 1},
                          {"capabilities": 0x200}, {"demo": True}):
            with self.subTest(overrides=overrides):
                client = ADCDiagnosticClient(**overrides)
                result = read_adc_diagnostics(client)
                self.assertFalse(result["supported"])
                self.assertEqual(client.calls, [(1, b"")])
                self.assertFalse(result["changed_device_settings"])
        # Even a contradictory INFO must not bypass the transport's demo flag.
        client = ADCDiagnosticClient(demo=True)
        client.identity["demo"] = False
        self.assertFalse(read_adc_diagnostics(client)["supported"])
        self.assertEqual(client.calls, [(1, b"")])

    def test_adc_diagnostics_through_readonly_client_sends_only_info_and_snapshot(self):
        class WireTransport(RecordingTransport):
            def _command(self, op, payload):
                if op == ADC_DIAGNOSTICS:
                    if payload:
                        raise ValueError("ADC diagnostics request must be empty")
                    return ADCDiagnosticClient().payload
                response = super()._command(op, payload)
                if op == 1:
                    info = list(struct.unpack("<8B5IH4B", response))
                    info[2] = 8
                    info[10] |= 0x80
                    return struct.pack("<8B5IH4B", *info)
                return response
        transport = WireTransport()
        client = KeyboardClient(transport, write_enabled=False)
        self.addCleanup(transport.close)
        client.output_enabled = transport.enabled = True
        result = read_adc_diagnostics(client)
        self.assertEqual(result["stop_reason_name"], "OWNERSHIP")
        self.assertEqual(transport.calls, [1, ADC_DIAGNOSTICS])
        self.assertEqual(client.sequence, 2)
        self.assertFalse(client.write_enabled)
        self.assertTrue(client.output_enabled)
        self.assertTrue(transport.enabled)

    def test_adc_diagnostics_payload_validation_and_unknown_reason(self):
        for length in (0, 4, 48, 51, 53, 56):
            client = ADCDiagnosticClient()
            client.payload = bytes(length)
            with self.subTest(length=length), self.assertRaisesRegex(ValueError, "52 bytes"):
                read_adc_diagnostics(client)
            self.assertEqual(client.calls, [(1, b""), (ADC_DIAGNOSTICS, b"")])
        client = ADCDiagnosticClient()
        values = list(struct.unpack("<13I", client.payload))
        values[0] = 2
        with self.assertRaisesRegex(ValueError, "running flag"):
            decode_adc_diagnostics(struct.pack("<13I", *values))
        values[0], values[7] = 1, 99
        unknown = decode_adc_diagnostics(struct.pack("<13I", *values))
        self.assertEqual(unknown["stop_reason"], 99)
        self.assertEqual(unknown["stop_reason_name"], "UNKNOWN")
        for reason, name in enumerate(("NONE", "FLAGS", "EPOCH", "OWNERSHIP", "RAW_RANGE",
                                       "FILTER", "ADC_OVR", "DMA")):
            values[7] = reason
            self.assertEqual(decode_adc_diagnostics(struct.pack("<13I", *values))["stop_reason_name"], name)

    def test_adc_diagnostics_tool_is_readonly_and_obeys_session_lifecycle(self):
        session, tools = self.diagnostic_tools()
        read = tools["keyboard_adc_diagnostics"]
        self.assertFalse(read()["supported"])  # Demo does not simulate hardware evidence.
        session.calibration = type("Calibration", (), {"active": True, "close": lambda self: None})()
        with patch("host.mcp_diagnostics.read_adc_diagnostics") as request:
            with self.assertRaises(DeviceError): read()
            request.assert_not_called()
        session.calibration = None
        session.disconnect()
        with self.assertRaises(DeviceError): read()

    def test_scheduler_diagnostics_wire_and_gate(self):
        values = (0xfffffffe, 2, 30, 4, 5, 6, 7, 15, 9, 10, 11, 12, 13)
        payload = struct.pack("<13I", *values)
        self.assertEqual(decode_scheduler_diagnostics(payload), dict(zip(SCHEDULER_FIELDS, values)))
        for size in (0, 48, 51, 53, 56):
            with self.assertRaises(ValueError): decode_scheduler_diagnostics(bytes(size))
        bad = list(values); bad[7] = 16
        with self.assertRaises(ValueError): decode_scheduler_diagnostics(struct.pack("<13I", *bad))
        for overrides in ({"patch": 8}, {"minor": 2, "patch": 99}, {"board_rev": 77},
                          {"role": 1}, {"capabilities": 0}, {"demo": True}):
            client = ADCDiagnosticClient(**overrides)
            self.assertFalse(read_scheduler_diagnostics(client)["supported"])
            self.assertEqual(client.calls, [(1, b"")])
        client = ADCDiagnosticClient(patch=9)
        client.payload = payload
        result = read_scheduler_diagnostics(client)
        self.assertTrue(result["supported"])
        self.assertEqual(result["micros"], 0xfffffffe)
        self.assertEqual(client.calls, [(1, b""), (SCHEDULER_DIAGNOSTICS, b"")])
        self.assertFalse(result["changed_output"])
        self.assertFalse(result["changed_firmware"])
        self.assertFalse(result["changed_device_settings"])
        # Transport demo flag wins over a contradictory INFO.
        client.transport.demo = True
        client.calls = []
        self.assertFalse(read_scheduler_diagnostics(client)["supported"])
        self.assertEqual(client.calls, [(1, b"")])

    def test_scheduler_through_readonly_transport_and_session(self):
        payload = struct.pack("<13I", 100, 2, 30, 4, 5, 6, 7, 3, 9, 10, 11, 12, 13)
        class WireTransport(RecordingTransport):
            def _command(self, op, data):
                if op == SCHEDULER_DIAGNOSTICS:
                    if data: raise ValueError("expected empty diagnostics request")
                    return payload
                response = super()._command(op, data)
                if op == 1:
                    response = bytearray(response); response[2] = 9
                    struct.pack_into("<I", response, 16, 0x3ff)
                return response
        transport = WireTransport()
        client = KeyboardClient(transport, write_enabled=False)
        self.assertTrue(read_scheduler_diagnostics(client)["supported"])
        self.assertEqual(transport.calls, [1, SCHEDULER_DIAGNOSTICS])
        session, tools = self.diagnostic_tools()
        read = tools["keyboard_scheduler_diagnostics"]
        self.assertFalse(read()["supported"])
        session.calibration = type("Calibration", (), {"active": True, "close": lambda self: None})()
        with patch("host.mcp_diagnostics.read_scheduler_diagnostics") as request:
            with self.assertRaises(DeviceError): read()
            request.assert_not_called()
        session.calibration = None; session.disconnect()
        with self.assertRaises(DeviceError): read()

    def test_adc_trace_decode_and_exact_key_selection(self):
        decoded = decode_adc_trace(trace_payload())
        self.assertEqual(decoded, {
            "sample_sequence": 0xfffffffe, "timestamp_us": 0x12345678, "keys": [5, 6],
            "rows": [
                {"key": 5, "raw_history": list(range(1000, 1008)), "averaged": 1004,
                 "after_deadband": 1005, "travel": 321, "pressed": True},
                {"key": 6, "raw_history": [4095] * 8, "averaged": 4095,
                 "after_deadband": 4094, "travel": 1000, "pressed": False}]})
        self.assertEqual(encode_adc_trace_keys([6, 0]), b"\x06\x00")
        self.assertEqual(decode_adc_trace(memoryview(b"abcd" + trace_payload())[4:]), decoded)
        for keys in (None, (), (5, 6), [], [5], [5, 6, 2], [5, 5], [-1, 6], [0, 7], [True, 6], [5., 6], ["5", 6]):
            client = ADCDiagnosticClient(capabilities=0x400)
            with self.subTest(keys=keys), self.assertRaises(ValueError): read_adc_trace(client, keys)
            self.assertEqual(client.calls, [])

    def test_adc_trace_malformed_response_and_preserved_filter_mismatch(self):
        for size in (0, 10, 52, 53, 55, 64):
            with self.subTest(size=size), self.assertRaisesRegex(ValueError, "54 bytes"):
                decode_adc_trace(bytes(size))
        for offset, value, width in ((8, 7, 1), (9, 5, 1),
                                     (10, 4096, 2), (26, 4096, 2), (28, 4096, 2),
                                     (32, 4096, 2), (48, 4096, 2), (50, 4096, 2),
                                     (30, 1001, 2), (52, 1023, 2),
                                     *[(30, 1 << bit, 2) for bit in range(10, 15)]):
            bad = bytearray(trace_payload())
            struct.pack_into("<B" if width == 1 else "<H", bad, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError): decode_adc_trace(bad)
        client = ADCDiagnosticClient(capabilities=0x400)
        client.payload = bytearray(trace_payload())
        struct.pack_into("<H", client.payload, 26, 2000)  # Plausible wire value; diagnostic inconsistency retained.
        result = read_adc_trace(client, [5, 6])
        self.assertEqual(result["rows"][0]["expected_rounded_average"], 1004)
        self.assertFalse(result["rows"][0]["average_matches_history"])
        self.assertEqual(result["rows"][0]["averaged"], 2000)

    def test_adc_trace_capability_demo_gate_and_no_implicit_commands(self):
        for overrides in ({"capabilities": 0x3ff}, {"board_rev": 77}, {"board_rev": 85}, {"role": 1}, {"demo": True}):
            client = ADCDiagnosticClient(capabilities=0x400)
            client.identity.update(overrides)
            if "demo" in overrides: client.transport.demo = overrides["demo"]
            self.assertFalse(read_adc_trace(client, [5, 6])["supported"])
            self.assertEqual(client.calls, [(1, b"")])
        client = ADCDiagnosticClient(capabilities=0x400, demo=True)
        client.identity["demo"] = False
        self.assertFalse(read_adc_trace(client, [5, 6])["supported"])
        self.assertEqual(client.calls, [(1, b"")])
        client = ADCDiagnosticClient(capabilities=0x400)
        client.payload = trace_payload()
        result = read_adc_trace(client, [5, 6])
        self.assertEqual(client.calls, [(1, b""), (ADC_TRACE, b"\x05\x06")])
        self.assertEqual(result["trace_requests"], 1)
        self.assertEqual(result["history_order"], "oldest_to_newest")
        self.assertTrue(all(row["average_matches_history"] for row in result["rows"]))
        for name in ("changed_device_settings", "changed_output", "changed_firmware"):
            self.assertFalse(result[name])
        client.calls = []
        with self.assertRaisesRegex(DeviceError, "requested order"): read_adc_trace(client, [6, 5])
        self.assertEqual(client.calls, [(1, b""), (ADC_TRACE, b"\x06\x05")])

    def test_adc_trace_full_report_readonly_transport_and_busy_no_retry(self):
        class WireTransport(RecordingTransport):
            busy = False
            def write(self, data):
                op, seq, _, _ = unpack_packet(data)
                if self.busy and op == ADC_TRACE:
                    self.calls.append(op)
                    self.queue.append(packet(op | 0x80, seq, b"", 5))
                    return
                super().write(data)
            def _command(self, op, payload):
                if op == ADC_TRACE:
                    self.asserted_keys = payload
                    return trace_payload()
                result = super()._command(op, payload)
                if op == 1:
                    result = bytearray(result)
                    struct.pack_into("<I", result, 16, 0x7ff)
                return result
        transport = WireTransport(); self.addCleanup(transport.close)
        client = KeyboardClient(transport, write_enabled=False)
        result = read_adc_trace(client, [5, 6])
        self.assertTrue(result["supported"])
        self.assertEqual(transport.calls, [1, ADC_TRACE])
        self.assertEqual(transport.asserted_keys, b"\x05\x06")
        self.assertEqual(client.sequence, 2)
        self.assertFalse(transport.enabled)
        # A device refusal propagates. In particular there is no OUTPUT or retry.
        transport.calls = []; transport.busy = True; transport.enabled = True
        with self.assertRaises(DeviceError): read_adc_trace(client, [5, 6])
        self.assertEqual(transport.calls, [1, ADC_TRACE])
        self.assertTrue(transport.enabled)

    def test_adc_trace_tool_obeys_session_lifecycle(self):
        session, tools = self.diagnostic_tools()
        read = tools["keyboard_adc_trace"]
        self.assertFalse(read([5, 6])["supported"])
        session.calibration = type("Calibration", (), {"active": True, "close": lambda self: None})()
        with patch("host.mcp_diagnostics.read_adc_trace") as request:
            with self.assertRaises(DeviceError): read([5, 6])
            request.assert_not_called()
        session.calibration = None; session.disconnect()
        with self.assertRaises(DeviceError): read([5, 6])

    def test_cpu_profile_codecs_unsigned_offset_and_stages(self):
        pages = cpu_profile_pages()
        self.assertEqual(decode_cpu_profile_begin(memoryview(b"xx\xfe\xff\xff\xff")[2:]), 0xfffffffe)
        expected = (0xfffffffe, 60000240, 240000000, 60000000, 5000, 2000, 12000000, 5001)
        self.assertEqual(decode_cpu_profile_page(memoryview(b"xx" + pages[0])[2:], 0),
                         dict(zip(CPU_PROFILE_FIELDS, expected)))
        for page in range(1, 5):
            result = decode_cpu_profile_page(pages[page], page)
            self.assertEqual(result["run_id"], 0xfffffffe)
            self.assertEqual([r["id"] for r in result["rows"]], list(range((page - 1) * 3, page * 3)))
            for row in result["rows"]:
                self.assertEqual(row["name"], CPU_PROFILE_STAGES[row["id"]])
                self.assertEqual(row["calls"], row["id"] + 10)
                self.assertEqual(row["total_cycles"], 2400 * (row["id"] + 1) if row["id"] != 11 else 0)

    def test_cpu_profile_malformed_pages_and_run_ids(self):
        for payload in (b"", bytes(3), bytes(4), bytes(5), None):
            with self.subTest(payload=payload), self.assertRaises(ValueError): decode_cpu_profile_begin(payload)
        for page in (True, -1, 5, 0., "1"):
            with self.subTest(page=page), self.assertRaises(ValueError): decode_cpu_profile_page(bytes(32), page)
        pages = cpu_profile_pages()
        for page in range(5):
            for length in (0, len(pages[page]) - 1, len(pages[page]) + 1):
                with self.subTest(page=page, length=length), self.assertRaises(ValueError):
                    decode_cpu_profile_page(bytes(length), page)
            bad = bytearray(pages[page]); bad[:4] = bytes(4)
            with self.assertRaisesRegex(ValueError, "run_id"): decode_cpu_profile_page(bad, page)
        for offset, value in ((4, 0), (8, 0), (12, 0), (4, 59999999), (12, 59999999)):
            bad = bytearray(pages[0]); struct.pack_into("<I", bad, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError): decode_cpu_profile_page(bad, 0)
        for offset, value in ((4, 0), (4, 2), (4, 4), (5, 1), (18, 0), (31, 12)):
            bad = bytearray(pages[1]); bad[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError): decode_cpu_profile_page(bad, 1)
        for offset in (36, 40):
            bad = bytearray(pages[4]); struct.pack_into("<I", bad, offset, 1)
            with self.assertRaisesRegex(ValueError, "zero timing"): decode_cpu_profile_page(bad, 4)

    def test_cpu_profile_run_lock_quiet_wait_conversion_and_nested_timings(self):
        client = CPUProfileClient()
        class ObservedLock:
            active = False
            def __enter__(self): self.active = True
            def __exit__(self, *_): self.active = False
        client.lock = ObservedLock()
        original_info, original_request = client.info, client.request
        def locked_info():
            self.assertTrue(client.lock.active)
            return original_info()
        def locked_request(*args):
            self.assertTrue(client.lock.active)
            return original_request(*args)
        waits = []
        def delay(seconds):
            self.assertTrue(client.lock.active)
            self.assertEqual(client.calls, [(1, b""), (CPU_PROFILE, b"\x00")])
            waits.append(seconds)
        client.info, client.request = locked_info, locked_request
        result = read_cpu_profile(client, delay=delay)
        self.assertFalse(client.lock.active)
        self.assertEqual(waits, [.35])
        self.assertEqual(client.calls, [(1, b""), (CPU_PROFILE, b"\x00")]
                         + [(CPU_PROFILE, bytes((1, page))) for page in range(5)])
        self.assertAlmostEqual(result["device_elapsed_seconds"], .250001)
        self.assertAlmostEqual(result["report_publication_hz"], 5000 / .250001)
        self.assertAlmostEqual(result["usb_completion_hz"], 2000 / .250001)
        self.assertAlmostEqual(result["irq_body_us"], 50000)
        self.assertEqual(result["stages"][0]["total_us"], 10)
        self.assertEqual(result["stages"][0]["mean_us"], 1)
        self.assertEqual(result["stages"][0]["max_us"], 1)
        self.assertAlmostEqual(result["stages"][0]["percent_elapsed"], 2400 * 100 / 60000240)
        self.assertEqual(result["stages"][9]["group"], "nested")
        self.assertEqual(result["stages"][11]["group"], "call_counter")
        self.assertFalse(result["stages"][11]["timing_available"])
        self.assertIsNone(result["stages"][11]["percent_elapsed"])
        self.assertIsNone(result["stages"][11]["mean_us"])
        self.assertEqual(result["profile_runs"], 1)
        self.assertNotIn("cpu_utilization", result)
        for name in ("changed_device_settings", "changed_output", "changed_firmware"):
            self.assertFalse(result[name])

    def test_cpu_profile_capability_demo_gate_before_begin(self):
        for overrides in ({"capabilities": 0x7ff}, {"board_rev": 77}, {"role": 1}, {"demo": True}):
            client = CPUProfileClient(); client.identity.update(overrides)
            with patch("host.mcp_diagnostics.time.sleep") as sleep:
                self.assertFalse(read_cpu_profile(client, delay=sleep)["supported"])
                sleep.assert_not_called()
            self.assertEqual(client.calls, [(1, b"")])
        client = CPUProfileClient(demo=True); client.identity["demo"] = False
        self.assertFalse(read_cpu_profile(client, delay=lambda _: self.fail("demo waited"))["supported"])
        self.assertEqual(client.calls, [(1, b"")])

    def test_cpu_profile_mid_read_replacement_and_busy_never_retry(self):
        for page in range(5):
            client = CPUProfileClient()
            bad = bytearray(client.pages[page]); struct.pack_into("<I", bad, 0, 4)
            client.pages[page] = bad
            with self.subTest(page=page), self.assertRaisesRegex(DeviceError, "run_id changed"):
                read_cpu_profile(client, delay=lambda _: None)
            self.assertEqual(len(client.calls), 3 + page)
        for failure in (b"\x00", b"\x01\x00", b"\x01\x03"):
            client = CPUProfileClient(); client.fail_at = failure; waits = []
            with self.subTest(failure=failure), self.assertRaisesRegex(DeviceError, "BUSY"):
                read_cpu_profile(client, delay=waits.append)
            self.assertEqual(client.calls[-1], (CPU_PROFILE, failure))
            self.assertEqual(client.calls.count((CPU_PROFILE, b"\x00")), 1)
            self.assertEqual(waits, [] if failure == b"\x00" else [.35])
            self.assertTrue(all(op in (1, CPU_PROFILE) for op, _ in client.calls))

    def test_cpu_profile_unused_stage_and_invalid_begin_are_bounded(self):
        client = CPUProfileClient()
        page = bytearray(client.pages[4])
        struct.pack_into("<3I", page, 19, 0, 0, 0)  # id10: no linear misses.
        client.pages[4] = page
        unused = read_cpu_profile(client, delay=lambda _: None)["stages"][10]
        self.assertEqual(unused["calls_per_second"], 0)
        self.assertEqual(unused["percent_elapsed"], 0)
        self.assertIsNone(unused["mean_us"])
        for begin in (b"", bytes(4), bytes(5)):
            client = CPUProfileClient(); client.begin = begin
            with self.subTest(begin=begin), self.assertRaises(ValueError):
                read_cpu_profile(client, delay=lambda _: self.fail("Invalid begin waited"))
            self.assertEqual(client.calls, [(1, b""), (CPU_PROFILE, b"\x00")])

    def test_cpu_profile_readonly_real_packet_exchange(self):
        pages = cpu_profile_pages()
        class WireTransport(RecordingTransport):
            def _command(self, op, payload):
                if op == CPU_PROFILE:
                    return struct.pack("<I", 0xfffffffe) if payload == b"\x00" else pages[payload[1]]
                result = super()._command(op, payload)
                if op == 1:
                    result = bytearray(result); struct.pack_into("<I", result, 16, 0xfff)
                return result
        transport = WireTransport(); self.addCleanup(transport.close)
        transport.enabled = True
        client = KeyboardClient(transport, write_enabled=False)
        client.output_enabled = True
        result = read_cpu_profile(client, delay=lambda _: None)
        self.assertTrue(result["supported"])
        self.assertEqual(transport.calls, [1] + [CPU_PROFILE] * 6)
        self.assertTrue(transport.enabled)
        self.assertTrue(client.output_enabled)

    def test_cpu_profile_tool_obeys_session_lifecycle(self):
        session, tools = self.diagnostic_tools()
        read = tools["keyboard_cpu_profile"]
        self.assertFalse(read()["supported"])
        session.calibration = type("Calibration", (), {"active": True, "close": lambda self: None})()
        with patch("host.mcp_diagnostics.read_cpu_profile") as request:
            with self.assertRaises(DeviceError): read()
            request.assert_not_called()
        session.calibration = None; session.disconnect()
        with self.assertRaises(DeviceError): read()

    def test_adc_irq_profile_decode_offsets_and_stage_partition(self):
        pages = adc_irq_profile_pages()
        self.assertEqual(decode_adc_irq_profile_begin(memoryview(b"xx\xf0\xff\xff\xff")[2:]), 0xfffffff0)
        expected = (0xfffffff0, 120000240, 240000000, 120000000, 500, 2, 10000, 4000, 32000, 0)
        self.assertEqual(decode_adc_irq_profile_page(memoryview(b"xx" + pages[0])[2:], 0),
                         dict(zip(ADC_IRQ_PROFILE_FIELDS, expected)))
        rows = []
        for page in (1, 2):
            decoded = decode_adc_irq_profile_page(memoryview(b"xx" + pages[page])[2:], page)
            self.assertEqual(decoded["run_id"], 0xfffffff0)
            self.assertEqual([r["id"] for r in decoded["rows"]], list(range((page - 1) * 3, page * 3)))
            rows.extend(decoded["rows"])
        self.assertEqual([r["name"] for r in rows], list(ADC_IRQ_PROFILE_STAGES))
        self.assertTrue(all(r["calls"] == 500 for r in rows))
        self.assertEqual(rows[0]["total_cycles"], sum(r["total_cycles"] for r in rows[1:]))
        # Maxima of different samples are deliberately not required to add.
        self.assertNotEqual(rows[0]["max_cycles"], sum(r["max_cycles"] for r in rows[1:]))

    def test_adc_irq_profile_rejects_bad_wire_lengths_reserved_and_ids(self):
        for begin in (b"", bytes(3), bytes(4), bytes(5), None):
            with self.subTest(begin=begin), self.assertRaises(ValueError): decode_adc_irq_profile_begin(begin)
        for page in (True, -1, 3, 1., "1"):
            with self.assertRaises(ValueError): decode_adc_irq_profile_page(bytes(40), page)
        pages = adc_irq_profile_pages()
        for page in range(3):
            for size in (0, len(pages[page]) - 1, len(pages[page]) + 1):
                with self.subTest(page=page, size=size), self.assertRaises(ValueError):
                    decode_adc_irq_profile_page(bytes(size), page)
            bad = bytearray(pages[page]); bad[:4] = bytes(4)
            with self.assertRaisesRegex(ValueError, "run_id"): decode_adc_irq_profile_page(bad, page)
        for offset, value in ((4, 0), (8, 0), (12, 0), (4, 119999999), (12, 119999999), (36, 1)):
            bad = bytearray(pages[0]); struct.pack_into("<I", bad, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError): decode_adc_irq_profile_page(bad, 0)
        for offset, value in ((4, 2), (4, 4), (5, 1), (18, 0), (31, 6)):
            bad = bytearray(pages[1]); bad[offset] = value
            with self.assertRaises(ValueError): decode_adc_irq_profile_page(bad, 1)

    def test_adc_irq_profile_serialized_quiet_run_and_sampled_metrics(self):
        client = ADCIRQProfileClient()
        class ObservedLock:
            active = False
            def __enter__(self): self.active = True
            def __exit__(self, *_): self.active = False
        client.lock = ObservedLock()
        original_info, original_request = client.info, client.request
        def locked_info():
            self.assertTrue(client.lock.active)
            return original_info()
        def locked_request(*args):
            self.assertTrue(client.lock.active)
            return original_request(*args)
        waits = []
        def delay(seconds):
            self.assertTrue(client.lock.active)
            self.assertEqual(client.calls, [(1, b""), (ADC_IRQ_PROFILE, b"\x00")])
            waits.append(seconds)
        client.info, client.request = locked_info, locked_request
        result = read_adc_irq_profile(client, delay=delay)
        self.assertEqual(waits, [.6])
        self.assertFalse(client.lock.active)
        self.assertEqual(client.calls, [(1, b""), (ADC_IRQ_PROFILE, b"\x00")]
                         + [(ADC_IRQ_PROFILE, bytes((1, page))) for page in range(3)])
        self.assertAlmostEqual(result["device_elapsed_seconds"], .500001)
        self.assertAlmostEqual(result["adc_scan_hz"], 32000 / .500001)
        self.assertAlmostEqual(result["report_publication_hz"], 10000 / .500001)
        self.assertAlmostEqual(result["usb_completion_hz"], 4000 / .500001)
        self.assertEqual(result["stages"][0]["mean_us"], 15)
        self.assertEqual(result["stages"][0]["max_us"], 37.5)
        self.assertEqual(result["stages"][2]["mean_us"], 2)
        self.assertEqual(result["partition_difference_cycles"], 0)
        self.assertTrue(result["partition_matches"])
        self.assertEqual(result["profiled_samples"], 500)
        self.assertEqual(result["discarded_samples"], 2)
        self.assertNotIn("cpu_utilization", result)
        self.assertTrue(all("percent_elapsed" not in row for row in result["stages"]))
        for name in ("changed_device_settings", "changed_output", "changed_firmware"):
            self.assertFalse(result[name])

    def test_adc_irq_profile_gates_before_begin_and_never_retries(self):
        for overrides in ({"capabilities": 0xfff}, {"board_rev": 77}, {"board_rev": 85}, {"role": 1}, {"demo": True}):
            client = ADCIRQProfileClient(); client.identity.update(overrides)
            self.assertFalse(read_adc_irq_profile(client, delay=lambda _: self.fail("Unsupported waited"))["supported"])
            self.assertEqual(client.calls, [(1, b"")])
        client = ADCIRQProfileClient(demo=True); client.identity["demo"] = False
        self.assertFalse(read_adc_irq_profile(client, delay=lambda _: self.fail("Demo waited"))["supported"])
        self.assertEqual(client.calls, [(1, b"")])
        for failed in (b"\x00", b"\x01\x00", b"\x01\x02"):
            client = ADCIRQProfileClient(); client.fail_at = failed; waits = []
            with self.assertRaisesRegex(DeviceError, "BUSY"):
                read_adc_irq_profile(client, delay=waits.append)
            self.assertEqual(client.calls[-1], (ADC_IRQ_PROFILE, failed))
            self.assertEqual(client.calls.count((ADC_IRQ_PROFILE, b"\x00")), 1)
            self.assertEqual(waits, [] if failed == b"\x00" else [.6])
            self.assertTrue(all(op in (1, ADC_IRQ_PROFILE) for op, _ in client.calls))
        for begin in (b"", bytes(4), bytes(5)):
            client = ADCIRQProfileClient(); client.begin = begin
            with self.assertRaises(ValueError):
                read_adc_irq_profile(client, delay=lambda _: self.fail("Invalid begin waited"))
            self.assertEqual(client.calls, [(1, b""), (ADC_IRQ_PROFILE, b"\x00")])

    def test_adc_irq_profile_cross_page_run_calls_and_mismatch_evidence(self):
        for page in range(3):
            client = ADCIRQProfileClient()
            bad = bytearray(client.pages[page]); struct.pack_into("<I", bad, 0, 12)
            client.pages[page] = bad
            with self.assertRaisesRegex(DeviceError, "run_id changed"):
                read_adc_irq_profile(client, delay=lambda _: None)
            self.assertEqual(len(client.calls), 3 + page)
        client = ADCIRQProfileClient()
        bad = bytearray(client.pages[1]); struct.pack_into("<I", bad, 6, 499)
        client.pages[1] = bad
        with self.assertRaisesRegex(DeviceError, "calls differ"):
            read_adc_irq_profile(client, delay=lambda _: None)
        client = ADCIRQProfileClient()
        bad = bytearray(client.pages[1]); struct.pack_into("<I", bad, 23, 120240)
        client.pages[1] = bad
        result = read_adc_irq_profile(client, delay=lambda _: None)
        self.assertFalse(result["partition_matches"])
        self.assertEqual(result["partition_difference_cycles"], 240)
        self.assertEqual(result["stages"][1]["total_cycles"], 120240)
        client = ADCIRQProfileClient(); client.pages = adc_irq_profile_pages(samples=0)
        empty = read_adc_irq_profile(client, delay=lambda _: None)
        self.assertEqual(empty["profiled_samples"], 0)
        self.assertTrue(all(row["mean_us"] is None for row in empty["stages"]))

    def test_adc_irq_profile_readonly_packet_exchange_and_session(self):
        pages = adc_irq_profile_pages()
        class WireTransport(RecordingTransport):
            def _command(self, op, payload):
                if op == ADC_IRQ_PROFILE:
                    return struct.pack("<I", 0xfffffff0) if payload == b"\x00" else pages[payload[1]]
                result = super()._command(op, payload)
                if op == 1:
                    result = bytearray(result); struct.pack_into("<I", result, 16, 0x1fff)
                return result
        transport = WireTransport(); self.addCleanup(transport.close)
        transport.enabled = True
        client = KeyboardClient(transport, write_enabled=False); client.output_enabled = True
        self.assertTrue(read_adc_irq_profile(client, delay=lambda _: None)["supported"])
        self.assertEqual(transport.calls, [1] + [ADC_IRQ_PROFILE] * 4)
        self.assertTrue(transport.enabled); self.assertTrue(client.output_enabled)
        session, tools = self.diagnostic_tools()
        read = tools["keyboard_adc_irq_profile"]
        self.assertFalse(read()["supported"])
        session.calibration = type("Calibration", (), {"active": True, "close": lambda self: None})()
        with patch("host.mcp_diagnostics.read_adc_irq_profile") as request:
            with self.assertRaises(DeviceError): read()
            request.assert_not_called()
        session.calibration = None; session.disconnect()
        with self.assertRaises(DeviceError): read()

    def test_interference_comparison_and_invalid_baseline_replacement(self):
        session, tools = self.diagnostic_tools()
        baseline, compare = tools['keyboard_interference_baseline'], tools['keyboard_interference_compare']
        with patch('host.mcp_diagnostics.capture_sensors', return_value=self.stats()):
            self.assertTrue(baseline(.2)['baseline_ready'])
        with patch('host.mcp_diagnostics.capture_sensors', return_value=self.stats(1000, 5)):
            result = compare(0, .2)
        self.assertTrue(result['sufficient_selected_motion'])
        self.assertEqual(len(result['other_keys']), 13)
        self.assertEqual(result['other_keys'][0]['relative_to_selected_percent'], .5)
        with patch('host.mcp_diagnostics.capture_sensors', return_value=self.stats(movement=65)):
            with self.assertRaisesRegex(DeviceError, 'movement'): baseline(.2)
        self.assertIsNone(session.interference)
        with self.assertRaisesRegex(DeviceError, 'baseline'): compare(0, .2)

    def test_interference_expiry_and_config_change_reject_before_sampling(self):
        session, tools = self.diagnostic_tools()
        baseline, compare = tools['keyboard_interference_baseline'], tools['keyboard_interference_compare']
        with patch('host.mcp_diagnostics.capture_sensors', return_value=self.stats()) as capture:
            baseline(.2)
            session.interference['created'] -= 301
            with self.assertRaisesRegex(DeviceError, 'baseline'): compare(0, .2)
            baseline(.2)
            session.interference['info']['config_crc'] ^= 1
            with self.assertRaisesRegex(DeviceError, 'changed'): compare(0, .2)
            self.assertEqual(capture.call_count, 2)
        self.assertIsNone(session.interference)


if __name__ == "__main__": unittest.main()
