import gzip
import json
import struct
import tempfile
import unittest
from pathlib import Path

from host import telemetry
from host.client import DeviceError, KeyboardClient
from host.protocol import (CAP_TELEMETRY, TELEMETRY_CONTROL, TELEMETRY_READ, ProtocolError, STATUSES,
                           packet, unpack_packet)
from host.telemetry_recorder import run


def read_reply(now, base, dropped, flags, records):
    body = b"".join(struct.pack("<hBH", delta, kind << 6 | index, value) for delta, kind, index, value in records)
    return struct.pack("<IIIBB", now, base, dropped, len(records), flags) + body


class CodecTests(unittest.TestCase):
    def test_control_encoding(self):
        self.assertEqual(telemetry.encode_control(None), b"")
        self.assertEqual(telemetry.encode_control(True, 6, 500), bytes([1, 6, 0xF4, 0x01]))
        self.assertEqual(telemetry.encode_control(False), bytes(4))
        for bad in ((True, 0, 100), (True, 4, 10001), (1, 4, 100)):
            with self.assertRaises(ProtocolError):
                telemetry.encode_control(*bad)
        reply = struct.pack("<BBHIIII", 1, 6, 500, 16384, 1234, 2, 7)
        self.assertEqual(telemetry.decode_control(reply), {"armed": True, "threshold": 6, "interval_us": 500,
                         "capacity": 16384, "device_now_us": 1234, "dropped": 2, "pending": 7})

    def test_read_layout_matches_firmware(self):
        # Same layout as src/telemetry.c telemetry_pack: header 14 bytes, records 5 bytes.
        payload = read_reply(5000, 0xFFFFFF00, 3, 3, [
            (0, 0, 9, 2047),                 # sample, key 9
            (0x100, 1, 9, 0x8000 | 6400),    # press decision (0..10000 travel), crosses 2^32
            (-50, 2, 0, 0x100 | 0xE1),       # USB: Left Shift pressed, slightly earlier
        ])
        result = telemetry.decode_read(payload)
        self.assertTrue(result["armed"] and result["more"])
        r = result["records"]
        self.assertEqual(r[0], {"device_us": 0xFFFFFF00, "kind": "sample", "key": 9, "raw": 2047})
        self.assertEqual(r[1], {"device_us": 0, "kind": "decision", "key": 9, "travel": 6400, "retriggered": False, "pressed": True})
        self.assertEqual(r[2], {"device_us": 0xFFFFFFCE, "kind": "usb", "usage": 0xE1, "pressed": True})

    def test_read_rejects_malformed(self):
        for payload in (b"\0" * 13, read_reply(0, 0, 0, 0, [(5, 0, 0, 1)]), read_reply(0, 0, 0, 4, []),
                        read_reply(0, 0, 0, 0, [(0, 0, 14, 1)]), read_reply(0, 0, 0, 0, [(0, 1, 0, 10001)]),
                        read_reply(0, 0, 0, 0, [(0, 3, 0, 0)]), read_reply(0, 0, 0, 0, [(0, 2, 0, 0x200)])):
            with self.assertRaises(ProtocolError):
                telemetry.decode_read(payload)

    def test_device_clock_unwrap(self):
        clock = telemetry.DeviceClock()
        self.assertEqual(clock.unwrap_now(0xFFFFFFF0), 0xFFFFFFF0)
        self.assertEqual(clock.unwrap_near(0x10), (1 << 32) + 0x10)       # record just after the wrap
        self.assertEqual(clock.unwrap_now(0x20), (1 << 32) + 0x20)
        self.assertEqual(clock.unwrap_near(0xFFFFFFF8), 0xFFFFFFF8)       # record just before it


class ClientGateTests(unittest.TestCase):
    def client(self, capabilities):
        sent = []
        class Transport:
            demo = False
            def write(self, raw):
                op, seq, status, data = unpack_packet(raw)
                sent.append(op)
                if op == 1:
                    info = bytearray(34); info[0:3] = bytes([0, 3, 44]); info[4:8] = bytes([14, 6, 4, 28])
                    struct.pack_into("<IIIII", info, 8, 96000, 125, capabilities, 0, 0)
                    struct.pack_into("<H", info, 28, 1024); info[30:34] = bytes([86, 1, 2, 1])
                    self.reply = packet(op | 128, seq, bytes(info))
                else:
                    self.reply = packet(op | 128, seq, struct.pack("<BBHIIII", 0, 4, 1000, 16384, 1, 0, 0))
            def read(self, timeout):
                return self.reply
        return KeyboardClient(Transport()), sent

    def test_requires_capability_and_no_write_permission(self):
        client, sent = self.client(CAP_TELEMETRY)
        self.assertFalse(telemetry.control(client)["armed"])     # read-only session is enough
        self.assertEqual(sent, [1, TELEMETRY_CONTROL])
        old, _ = self.client(0)
        with self.assertRaises(ProtocolError):
            telemetry.control(old)


class PipelineTests(unittest.TestCase):
    def test_keeps_requests_in_flight_and_matches_sequences(self):
        class Transport:
            def __init__(self):
                self.queue, self.writes = [], []
            def write(self, raw):
                op, seq, status, data = unpack_packet(raw)
                assert op == TELEMETRY_READ and seq >= 0x8000 and data == b""
                self.writes.append(seq)
                self.queue.append(packet(op | 128, seq, read_reply(seq, seq, 0, 1, [(0, 0, 1, seq & 0xFFF)])))
            def read(self, timeout):
                return self.queue.pop(0) if self.queue else b""
        class Client:
            identified = True
            device_info = {"capabilities": CAP_TELEMETRY}
            transport = Transport()
        client = Client()
        reader = telemetry.PipelinedReader(client, depth=3)
        ticks = iter(range(1, 1000))
        first = reader.read(lambda: next(ticks))
        self.assertEqual(len(client.transport.writes), 3)            # three requests went out at once
        self.assertEqual(first[2]["device_now_us"], client.transport.writes[0])
        second = reader.read(lambda: next(ticks))
        self.assertEqual(second[2]["device_now_us"], client.transport.writes[1])
        self.assertEqual(len(client.transport.writes), 4)            # topped back up to three in flight
        reader.drain()
        self.assertEqual(reader.outstanding, {})
        Client.device_info = {"capabilities": 0}
        with self.assertRaises(ProtocolError):
            telemetry.PipelinedReader(Client())


class RecorderTests(unittest.TestCase):
    def test_records_sync_and_disarm(self):
        class FakeClient:
            def __init__(self):
                self.calls = []
                self.reads = [read_reply(1000, 900, 0, 3, [(0, 0, 2, 1500), (50, 1, 2, 0x8000 | 300)]),
                              read_reply(1100, 1050, 0, 1, [(0, 2, 0, 0x100 | 0x16)]),
                              "NOT_READY",
                              read_reply(2000, 2000, 1, 1, [])]
            def info(self):
                return {"capabilities": CAP_TELEMETRY, "version": "0.3.43", "demo": False}
            def get_config(self):
                return {"keys": [], "bindings": []}
            def request(self, op, payload=b""):
                self.calls.append((op, payload))
                if op == TELEMETRY_CONTROL:
                    return struct.pack("<BBHIIII", payload[0] if payload else 0, 4, 1000, 16384, 0, 0, 0)
                reply = self.reads.pop(0)
                if reply == "NOT_READY":
                    raise DeviceError(STATUSES[7])
                return reply
        class FakeReader:
            def __init__(self, client):
                self.client, self.drains = client, 0
            def read(self, clock_ns):
                sent = clock_ns()
                reply = telemetry.decode_read(self.client.request(TELEMETRY_READ))
                return sent, clock_ns(), reply
            def drain(self):
                self.drains += 1
        client = FakeClient()
        ticks = iter(range(10, 10_000, 10))
        readers = []
        with tempfile.TemporaryDirectory() as out:
            folder, totals = run(client, out, 4, 1000, stop=lambda: not client.reads, clock_ns=lambda: next(ticks),
                                 reader_factory=lambda c: readers.append(FakeReader(c)) or readers[-1])
            self.assertEqual(readers[0].drains, 2)          # before re-arming, and on exit
            self.assertEqual(totals, {"records": 3, "reads": 3, "dropped": 1, "rearmed": 1})
            rows = gzip.open(folder / "records.csv.gz", "rt").read().splitlines()
            self.assertEqual(rows[1:], ["900,sample,2,,1500,,,", "950,decision,2,,,300,1,0", "1050,usb,,22,,,1,"])
            sync = gzip.open(folder / "sync.csv.gz", "rt").read().splitlines()
            self.assertEqual(len(sync), 4)
            session = json.loads((folder / "session.json").read_text(encoding="utf-8"))
            self.assertEqual(session["totals"]["records"], 3)
            self.assertFalse(session["disarmed"]["armed"])
        controls = [payload for op, payload in client.calls if op == TELEMETRY_CONTROL]
        self.assertEqual(controls, [bytes([1, 4, 0xE8, 3]), bytes([1, 4, 0xE8, 3]), bytes(4)])


if __name__ == "__main__":
    unittest.main()
