"""MCP maintenance guards and verified updater integration, all in memory."""
import copy
import json
from pathlib import Path
import tempfile
import threading
import unittest

from host.mcp_maintenance import read_package, update_app
from host.session import KeyboardSession
from host.simulator import defaults
from host.update_firmware import FirmwareUpdater, BEGIN, WRITE
from tests.host_update_test import Transport, package


class Client:
    def __init__(self, fault=None):
        self.transport = Transport(fault)
        self.transport.demo = False
        self.write_enabled = True
        self.lock = threading.RLock()
        self.profile = defaults()
        self.dirty = False
        self.output_calls = []
    def info(self):
        return dict(board_rev=86, capabilities=0x40)
    def status(self, side):
        return dict(dirty=self.dirty, calibration_state=0, fresh=True)
    def get_config(self):
        return copy.deepcopy(self.profile)
    def output(self, enabled):
        self.output_calls.append(enabled)
        return dict(output_enabled=enabled)


class MaintenanceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name)/"test.fw.json"
        self.pkg = package()
        self.path.write_text(json.dumps(self.pkg), encoding="utf-8")
        self.client = Client()
        self.session = KeyboardSession(client=self.client, firmware_write_enabled=True)
    def tearDown(self):
        self.tmp.cleanup()
    def update(self, **overrides):
        args = dict(package_path=str(self.path), target="left", expected_sha256=self.pkg["sha256"], confirm_update=True)
        args.update(overrides)
        return update_app(self.session, **args)
    def test_inspect_does_not_open_device_and_omits_image(self):
        _, m = read_package(self.path)
        self.assertTrue(m["validated"])
        self.assertFalse(m["device_opened"])
        self.assertNotIn("image_base64", m)
        self.assertEqual(m["image_sha256"], self.pkg["sha256"])
        self.assertFalse(self.client.transport.calls)
    def test_confirmation_permission_role_and_hash_fail_before_any_write(self):
        for override in (dict(confirm_update=False), dict(confirm_update=1), dict(target="right"), dict(expected_sha256="0"*64)):
            with self.subTest(override=override), self.assertRaises((ValueError, PermissionError)):
                self.update(**override)
        self.session.firmware_write_enabled = False
        with self.assertRaises(PermissionError):
            self.update()
        self.assertFalse(self.client.transport.calls)
        self.assertFalse(self.client.output_calls)
    def test_unsaved_settings_prevent_reboot(self):
        self.client.dirty = True
        with self.assertRaisesRegex(ValueError, "Unsaved"):
            self.update()
        self.assertFalse(self.client.transport.calls)
        self.assertFalse(self.client.output_calls)
    def test_real_updater_protocol_mock_verifies_image_and_settings(self):
        result = self.update()
        self.assertTrue(result["firmware_verified"])
        self.assertTrue(result["settings_verified_unchanged"])
        self.assertTrue(result["full_image_readback"])
        self.assertEqual(self.client.output_calls, [False])
        self.assertEqual(sum(op == BEGIN for op, _, _ in self.client.transport.calls), 1)
        json.dumps(result)  # No bytes leak into MCP structured output.
    def test_configuration_changed_at_output_release_blocks_updater(self):
        output = self.client.output
        def changed(enabled):
            result = output(enabled)
            self.client.dirty = True
            self.client.profile["keys"][0]["actuation"] = 400
            return result
        self.client.output = changed
        with self.assertRaisesRegex(RuntimeError, "preflight"):
            self.update()
        self.assertFalse(self.client.transport.calls)
    def test_failed_write_not_retried(self):
        self.client.transport.fault = "write"
        with self.assertRaisesRegex(RuntimeError, "No automatic retry"):
            self.update()
        self.assertEqual(sum(op == WRITE for op, _, _ in self.client.transport.calls), 1)
    def test_readback_difference_is_not_success(self):
        class ChangesSettings(FirmwareUpdater):
            def update(inner, *args, **kwargs):
                result = super().update(*args, **kwargs)
                self.client.profile["keys"][0]["actuation"] = 300
                return result
        r = self.update(updater_factory=ChangesSettings)
        self.assertTrue(r["firmware_verified"])
        self.assertFalse(r["settings_verified_unchanged"])
        self.assertIn("differs", r["warning"])


if __name__ == "__main__":
    unittest.main()
