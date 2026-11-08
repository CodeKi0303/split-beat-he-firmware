"""Offline update protocol/state tests. Never imports HID drivers or opens USB."""
import base64
import binascii
import hashlib
import struct
import unittest

from host.update_firmware import *
from host.protocol import packet, unpack_packet


def package(role=0):
    image = bytearray(b"\xff" * 0x440)
    struct.pack_into("<8sBBH5I", image, 0x400, b"C86FW003", 86, role, 1, 0x30000, APP_BASE, MAX_IMAGE, 0x33444946, 0xCCBBB6B9)
    return dict(schema="c86-firmware-v1", hardware=86, role="right" if role else "left", version="0.3.0", version_number=0x30000, image_base=APP_BASE, image_size=len(image), crc32=binascii.crc32(image), sha256=hashlib.sha256(image).hexdigest(), image_base64=base64.b64encode(image).decode())


class Transport:
    def __init__(self, fault=None):
        self.calls, self.response, self.fault = [], b"", fault
        self.boards = [dict(role=r, mode=1, flags=3, size=0, crc=0, version=0x30000, session=0, offset=0, uid=bytes([r + 1])*12, image=bytearray()) for r in (0, 1)]
    def write(self, data):
        wire, seq, _, p = unpack_packet(data)
        role = int(wire >= 0x50)
        op, b = wire - (0x20 if role else 0), self.boards[role]
        self.calls.append((op, role, p))
        out, status = b"", 0
        if self.fault == "old":
            self.response = packet(wire | 128, seq, status=1)
            return
        if op == INFO:
            out = struct.pack("<4B2H7I12s", 1, 86, role, b["mode"], 1024, b["flags"], APP_BASE, MAX_IMAGE, b["size"], b["crc"], b["version"], b["session"], b["offset"], b["uid"])
        elif op == ENTER:
            b["mode"] = 0
            if self.fault == "uid":
                b["uid"] = b"x"*12
        elif op == BEGIN:
            uid, hw, dest, reserved, session, size, crc, version = struct.unpack("<12sBBH4I", p)
            assert (uid, hw, dest, reserved) == (b["uid"], 86, role, 0)
            b.update(session=session, size=size, crc=crc, version=version, offset=0, flags=1, image=bytearray(size))
            if self.fault == "begin":
                raise TimeoutError("Lost BEGIN ACK")
        elif op == WRITE:
            session, offset = struct.unpack_from("<2I", p)
            assert session == b["session"] and offset == b["offset"]
            b["image"][offset:offset+32] = p[8:]
            b["offset"] += 32
            if self.fault == "write":
                raise TimeoutError("Lost WRITE ACK")
        elif op == COMMIT:
            assert binascii.crc32(b["image"]) == b["crc"]
            b["flags"] |= 2
        elif op == READ:
            offset, n = struct.unpack("<IB", p)
            out = bytes(b["image"][offset:offset+n])
            if self.fault == "read":
                out = bytes([out[0] ^ 1]) + out[1:]
        elif op == RUN:
            assert b["flags"] & 2
            b["mode"] = 1
        else:
            status = 1
        self.response = packet(wire | 128, seq, out, status)
    def read(self, timeout):
        r, self.response = self.response, b""
        return r
    def reconnect(self, timeout=15):
        pass


class ReenumeratingTransport(Transport):
    """The first reopen finds the old handle; the next sees no interface yet."""
    def __init__(self, *, lose_ack=False, never_reappear=False, stale_reply=False, fault=None):
        super().__init__(fault)
        self.lose_ack, self.never_reappear = lose_ack, never_reappear
        self.stale_reply = stale_reply
        self.reconnects, self.transition_attempts, self.stale = [], None, False

    def write(self, data):
        op, _, _, _ = unpack_packet(data)
        if op == INFO and self.stale:
            if self.stale_reply:
                super().write(data)
                wire, seq, status, payload = unpack_packet(self.response)
                payload = payload[:3] + bytes([1 - payload[3]]) + payload[4:]
                self.response = packet(wire, seq, payload, status)
                return
            raise DeviceError("device disconnected")
        super().write(data)
        if op in (ENTER, RUN):
            self.transition_attempts, self.stale = 0, True
            if self.lose_ack:
                self.response = b""
                raise OSError("USB reset lost the mode-change ACK")

    def reconnect(self, timeout=15):
        self.reconnects.append(timeout)
        self.transition_attempts += 1
        if self.never_reappear or self.transition_attempts == 2:
            raise DeviceError("Expected one vendor HID; found 0")
        self.stale = self.transition_attempts < 3


class FirmwareTests(unittest.TestCase):
    def test_both_targets_full_readback(self):
        for role in (0, 1):
            t = Transport()
            u = FirmwareUpdater(t, write_enabled=True, delay=lambda _: None)
            r = u.update(package(role), target=role, full_readback=True, session=0x12345678)
            self.assertEqual(r["mode"], 1)
            self.assertEqual(r["version"], 0x30000)
            self.assertEqual(sum(op == WRITE for op, _, _ in t.calls), 0x440//32)
            self.assertEqual(sum(op == READ for op, _, _ in t.calls), (0x440+47)//48)
            self.assertEqual(t.calls[-1][:2], (RUN, 0) if role else (INFO, 0))
            self.assertTrue(all(op >= 0x30 for op, _, _ in t.calls))

    def test_no_implicit_write_and_wrong_side(self):
        t = Transport()
        u = FirmwareUpdater(t)
        with self.assertRaises(PermissionError):
            u.update(package(), target=0)
        with self.assertRaises(PermissionError):
            u.info(1)
        self.assertFalse(t.calls)
        u.write_enabled = True
        with self.assertRaises(ValueError):
            u.update(package(), target=1)
        self.assertFalse(t.calls)

    def test_corrupt_package_no_device_access(self):
        for field, value in (("sha256", "0"*64), ("crc32", 0), ("image_size", 1), ("image_base", 0), ("image_base64", "!!!"), ("version_number", True)):
            p, t = package(), Transport()
            p[field] = value
            with self.assertRaises(ValueError):
                FirmwareUpdater(t, write_enabled=True).update(p, target=0)
            self.assertFalse(t.calls)
        p = package()
        image = bytearray(base64.b64decode(p["image_base64"]))
        image[0x409] = 1
        p.update(image_base64=base64.b64encode(image).decode(), crc32=binascii.crc32(image), sha256=hashlib.sha256(image).hexdigest())
        with self.assertRaisesRegex(ValueError, "identity"):
            validate_package(p)

    def test_errors_do_not_retry_erase_or_run(self):
        for fault in ("old", "uid", "begin", "write", "read"):
            t = Transport(fault)
            with self.assertRaises((ValueError, RuntimeError, TimeoutError)):
                FirmwareUpdater(t, write_enabled=True, delay=lambda _: None).update(package(), target=0, full_readback=True)
            self.assertLessEqual(sum(op == BEGIN for op, _, _ in t.calls), 1)
            self.assertFalse(any(op == RUN for op, _, _ in t.calls))
            if fault in ("old", "uid"):
                self.assertFalse(any(op == BEGIN for op, _, _ in t.calls))

    def test_boot_recovery_does_not_need_valid_old_app(self):
        t = Transport()
        t.boards[0].update(mode=0, flags=1)
        FirmwareUpdater(t, write_enabled=True).update(package(), target=0)
        self.assertFalse(any(op == ENTER for op, _, _ in t.calls))

    def test_invalid_app_cannot_leave(self):
        t = Transport()
        t.boards[0].update(mode=0, flags=1)
        with self.assertRaises(RuntimeError):
            FirmwareUpdater(t, write_enabled=True).leave_maintenance()
        self.assertFalse(any(op == RUN for op, _, _ in t.calls))

    def test_previous_session_requires_power_cycle(self):
        t = Transport()
        t.boards[0].update(mode=0, flags=1, session=7, offset=32)
        with self.assertRaisesRegex(RuntimeError, "Fully remove"):
            FirmwareUpdater(t, write_enabled=True).update(package(), target=0)
        self.assertFalse(any(op == BEGIN for op, _, _ in t.calls))

    def test_local_reenumeration_reopens_stale_handle_without_replaying_commands(self):
        for lose_ack, stale_reply in ((False, False), (True, False), (False, True)):
            t = ReenumeratingTransport(lose_ack=lose_ack, stale_reply=stale_reply)
            after = FirmwareUpdater(t, write_enabled=True, delay=lambda _: None).update(
                package(), target=0, full_readback=True, session=0x12345678)
            self.assertEqual(after["mode"], 1)
            for op in (ENTER, BEGIN, COMMIT, RUN):
                self.assertEqual(sum(call[0] == op for call in t.calls), 1)
            self.assertEqual(sum(call[0] == WRITE for call in t.calls), 0x440 // 32)
            self.assertEqual(len(t.reconnects), 6)  # Three opens per ENTER / RUN.
            self.assertTrue(all(0 < timeout <= 1 for timeout in t.reconnects))

    def test_reenumeration_still_rejects_changed_uid_before_erase(self):
        t = ReenumeratingTransport(fault="uid")
        with self.assertRaisesRegex(ValueError, "UID"):
            FirmwareUpdater(t, write_enabled=True, delay=lambda _: None).update(package(), target=0)
        self.assertEqual(len(t.reconnects), 3)
        self.assertEqual(sum(call[0] == ENTER for call in t.calls), 1)
        self.assertFalse(any(call[0] in (BEGIN, WRITE, COMMIT, RUN) for call in t.calls))

    def test_reenumeration_has_bounded_deadline_and_never_erases_when_absent(self):
        t = ReenumeratingTransport(never_reappear=True)
        now = [0.0]
        def advance(_):
            now[0] += 5.0
        with self.assertRaisesRegex(RuntimeError, "Mode transition not confirmed"):
            FirmwareUpdater(t, write_enabled=True, delay=advance, clock=lambda: now[0]).update(package(), target=0)
        self.assertEqual(len(t.reconnects), 4)
        self.assertEqual(sum(call[0] == ENTER for call in t.calls), 1)
        self.assertFalse(any(call[0] in (BEGIN, WRITE, COMMIT, RUN) for call in t.calls))


if __name__ == "__main__":
    unittest.main()
