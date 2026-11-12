"""Configuration MCP contracts exercised only through the no-USB simulator."""
from copy import deepcopy
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
from types import SimpleNamespace
import unittest

from host.client import DeviceError, KeyboardClient
from host.mcp_configuration import (
    apply_records, binding_catalog, layout, make_studio_profile, parse_studio_profile,
    profile_fingerprint, propose_bindings, propose_keys, register_configuration_tools,
)
from host.protocol import DEFAULTS, INFO, MUTATIONS, OUTPUT, SAVE, SET_BINDING, SET_KEY, ProtocolError
from host.simulator import DemoTransport, defaults


class RecordingTransport(DemoTransport):
    def __init__(self, capabilities=0x13f):
        super().__init__()
        self.commands = []
        self.capabilities = capabilities
        for key in self.profile["keys"]:
            key.update(flags=7, released=1000, pressed=3000)
        # Opposite-polarity keys must keep their own calibration endpoints.
        self.profile["keys"][7].update(released=3200, pressed=900)

    def _command(self, op, payload):
        self.commands.append((op, payload))
        result = super()._command(op, payload)
        if op == INFO:
            result = bytearray(result)
            struct.pack_into("<I", result, 16, self.capabilities)
            result = bytes(result)
        return result

    def mutations(self):
        return [op for op, _ in self.commands if op in MUTATIONS]


def setup_tools(write=True, capabilities=0x13f):
    transport = RecordingTransport(capabilities)
    client = KeyboardClient(transport, write_enabled=write)
    session = SimpleNamespace(client=client, travel_mm=[4.] * 14, calibration=None)
    session.require_client = lambda: session.client
    registered = {}

    class Server:
        def tool(self, **kwargs):
            def decorate(fn):
                registered[fn.__name__] = fn
                return fn
            return decorate

    register_configuration_tools(Server(), session, {}, {}, lambda fn: fn)
    return transport, client, session, registered


class ConfigurationProposalTests(unittest.TestCase):
    def setUp(self):
        self.transport, self.client, self.session, self.tools = setup_tools()
        self.profile = deepcopy(self.transport.profile)

    def test_mm_multi_patch_preserves_individual_calibration_and_other_fields(self):
        travel = [4.] * 14
        travel[7] = 3.2
        proposed = propose_keys(self.profile, [0, 7], {"actuation": .8, "rt_release": .12},
                                unit="mm", travel_mm=travel, capabilities=0x13f)
        self.assertEqual([k["actuation"] for k in proposed], [200, 250])
        self.assertEqual([k["rt_release"] for k in proposed], [30, 38])
        for key in proposed:
            expected = deepcopy(self.profile["keys"][key["key"]])
            expected.update(actuation=key["actuation"], rt_release=key["rt_release"])
            self.assertEqual(key, expected)
        self.assertEqual(self.transport.mutations(), [])

    def test_patch_keeps_calibration_flag_and_all_unspecified_settings(self):
        result = propose_keys(self.profile, [2], {"rapid_trigger": False, "enabled": False}, capabilities=0x13f)[0]
        expected = {**self.profile["keys"][2], "flags": 4}
        self.assertEqual(result, expected)

    def test_strict_selection_and_patch_validation(self):
        invalid = [([], {"actuation": 300}, "normalized"),
                   ([0, 0], {"actuation": 300}, "normalized"),
                   ([True], {"actuation": 300}, "normalized"),
                   ([14], {"actuation": 300}, "normalized"),
                   ([0], {}, "normalized"),
                   ([0], {"released": 100}, "normalized"),
                   ([0], {"enabled": 1}, "normalized"),
                   ([0], {"actuation": True}, "normalized"),
                   ([0], {"actuation": 300.0}, "normalized"),
                   ([0], {"actuation": .301}, "mm"),
                   ([0], {"actuation": .01, "top_deadzone": .02}, "mm"),
                   ([0], {"actuation": 3.5, "bottom_deadzone": .51}, "mm"),
                   ([0], {"rt_press": 4.01}, "mm"),
                   ([0], {"rt_press": 0}, "normalized"),
                   ([0], {"actuation": 200}, "metres")]
        for keys, patch, unit in invalid:
            with self.subTest(keys=keys, patch=patch, unit=unit), self.assertRaises(ProtocolError):
                propose_keys(self.profile, keys, patch, unit=unit, capabilities=0x13f)

    def test_uncalibrated_distances_blocked_but_safe_disable_possible(self):
        profile = defaults()
        with self.assertRaisesRegex(ProtocolError, "calibrate"):
            propose_keys(profile, [0], {"actuation": 300}, capabilities=0x13f)
        with self.assertRaisesRegex(ProtocolError, "calibrate"):
            propose_keys(profile, [0], {"enabled": True}, capabilities=0x13f)
        self.assertEqual(propose_keys(profile, [0], {"enabled": False})[0]["flags"], 2)
        self.assertEqual(propose_keys(profile, [0], {"noise_deadband": 8})[0]["noise_deadband"], 8)

    def test_readonly_preview_does_not_write(self):
        transport, _, _, tools = setup_tools(write=False)
        result = tools["keyboard_preview_keys"]([0, 1], {"actuation": 250})
        self.assertEqual(result["fingerprint"], profile_fingerprint(transport.profile))
        self.assertEqual(len(result["proposed_keys"]), 2)
        self.assertFalse(result["device_written"])
        self.assertEqual(transport.mutations(), [])


class ConfigurationApplyTests(unittest.TestCase):
    def setUp(self):
        self.transport, self.client, self.session, self.tools = setup_tools()
        self.original = deepcopy(self.transport.profile)

    def test_group_apply_only_selected_and_explicit_output_off(self):
        result = self.tools["keyboard_set_keys"]([0, 7], {"actuation": 240, "bottom_deadzone": 10})
        self.assertTrue(result["complete"])
        self.assertEqual(result["writes_attempted"], 2)
        self.assertEqual(self.transport.mutations(), [OUTPUT, SET_KEY, SET_KEY])
        self.assertIn((OUTPUT, b"\0"), self.transport.commands)
        self.assertFalse(result["persisted"])
        expected = deepcopy(self.original)
        for key in (0, 7):
            expected["keys"][key].update(actuation=240, bottom_deadzone=10)
        self.assertEqual(result["profile"], expected)
        self.assertEqual(self.transport.generation, 0)

    def test_bindings_keyboard_mouse_wheel_with_full_profile_preservation(self):
        selected = [dict(input=1, kind=1, code=4, modifiers=3),
                    dict(input=16, kind=5, code=3, modifiers=0),
                    dict(input=24, kind=3, code=-1, modifiers=0)]
        result = self.tools["keyboard_set_bindings"](selected)
        self.assertTrue(result["complete"])
        expected = deepcopy(self.original)
        for record in selected:
            expected["bindings"][record["input"]] = record
        self.assertEqual(result["profile"], expected)
        self.assertEqual(self.transport.mutations(), [OUTPUT, SET_BINDING, SET_BINDING, SET_BINDING])

    def test_invalid_last_binding_rejects_entire_batch_before_output(self):
        bindings = [dict(input=0, kind=1, code=4, modifiers=0), dict(input=1, kind=5, code=6, modifiers=0)]
        with self.assertRaises(ProtocolError):
            self.tools["keyboard_set_bindings"](bindings)
        self.assertEqual(self.transport.mutations(), [])

    def test_old_firmware_preflights_capabilities_before_any_mutation(self):
        transport, _, _, tools = setup_tools(capabilities=0x1f)
        with self.assertRaisesRegex(ProtocolError, "Bottom deadzone"):
            tools["keyboard_set_keys"]([0, 1], {"bottom_deadzone": 50})
        with self.assertRaisesRegex(ProtocolError, "firmware"):
            tools["keyboard_set_bindings"]([dict(input=1, kind=5, code=1, modifiers=0)])
        self.assertEqual(transport.mutations(), [])

    def test_stale_preview_fingerprint_and_concurrent_change_are_not_overwritten(self):
        target = propose_keys(self.original, [0], {"actuation": 250}, capabilities=0x13f)
        self.transport.profile["keys"][3]["actuation"] = 220
        with self.assertRaisesRegex(DeviceError, "changed since preview"):
            self.tools["keyboard_set_keys"]([0], {"actuation": 250}, expected_fingerprint=profile_fingerprint(self.original))
        result = apply_records(self.client, self.original, keys=target)
        self.assertFalse(result["complete"])
        self.assertTrue(result["unexpected_changes"])
        self.assertEqual(self.transport.mutations(), [])

    def test_write_failure_reports_exact_partial_state_without_retry_or_save(self):
        original_set = self.client.set_key
        def fail_second(record):
            if record["key"] == 7:
                raise DeviceError("injected failure")
            return original_set(record)
        self.client.set_key = fail_second
        result = self.tools["keyboard_set_keys"]([0, 7], {"actuation": 250})
        self.assertFalse(result["complete"])
        self.assertEqual(result["verified_records"], [{"group": "keys", "index": 0}])
        self.assertEqual(result["writes_attempted"], 2)
        self.assertEqual(result["profile"]["keys"][7], self.original["keys"][7])
        self.assertIn("injected failure", result["error"])
        self.assertEqual(self.transport.mutations(), [OUTPUT, SET_KEY])
        self.assertFalse(result["unexpected_changes"])

    def test_lost_ack_can_only_succeed_after_exact_full_readback(self):
        original_set = self.client.set_key
        def lose_ack(record):
            original_set(record)
            raise TimeoutError("ack lost")
        self.client.set_key = lose_ack
        result = self.tools["keyboard_set_keys"]([0], {"actuation": 250})
        self.assertTrue(result["complete"])
        self.assertEqual(result["writes_attempted"], 1)
        self.assertEqual(self.transport.mutations(), [OUTPUT, SET_KEY])

    def test_output_ack_failure_never_sends_configuration_writes(self):
        self.client.output = lambda enabled: {"output_enabled": True}
        result = self.tools["keyboard_set_keys"]([0], {"actuation": 250})
        self.assertFalse(result["complete"])
        self.assertFalse(result["output_off_acknowledged"])
        self.assertEqual(self.transport.mutations(), [])

    def test_unrelated_changed_binding_detected_during_final_readback(self):
        original_set = self.client.set_key
        def concurrent_edit(record):
            result = original_set(record)
            self.transport.profile["bindings"][18] = dict(input=18, kind=1, code=90, modifiers=0)
            return result
        self.client.set_key = concurrent_edit
        result = self.tools["keyboard_set_keys"]([0], {"actuation": 250})
        self.assertFalse(result["complete"])
        self.assertTrue(result["unexpected_changes"])

    def test_missing_final_readback_is_explicitly_uncertain(self):
        original_get = self.client.get_config
        original_set = self.client.set_key
        changed = False
        def get():
            if changed:
                raise OSError("disconnected")
            return original_get()
        def set_key(record):
            nonlocal changed
            result = original_set(record)
            changed = True
            return result
        self.client.get_config = get
        self.client.set_key = set_key
        result = self.tools["keyboard_set_keys"]([0], {"actuation": 250})
        self.assertFalse(result["complete"])
        self.assertIsNone(result["profile"])
        self.assertIn("Some settings may have changed", result["error"])
        self.assertEqual(self.transport.mutations(), [OUTPUT, SET_KEY])

    def test_raw_records_cannot_overwrite_calibration_or_duplicate_inputs(self):
        key = {**self.original["keys"][0], "released": 900}
        with self.assertRaisesRegex(ProtocolError, "measured calibration"):
            apply_records(self.client, self.original, keys=[key])
        with self.assertRaisesRegex(ProtocolError, "Duplicate"):
            propose_bindings([self.original["bindings"][0]] * 2, 0x13f)
        with self.assertRaisesRegex(ProtocolError, "lists"):
            apply_records(self.client, self.original, keys={})
        self.assertEqual(self.transport.mutations(), [])


class ConfigurationStudioToolsTests(unittest.TestCase):
    def setUp(self):
        self.transport, self.client, self.session, self.tools = setup_tools()

    def test_studio_roundtrip_and_raw_legacy_zero_bottom_compatibility(self):
        self.tools["keyboard_set_travel_mm"]([0, 7], 3.2)
        exported = self.tools["keyboard_export_studio_profile"]()
        target, travel = parse_studio_profile(exported)
        self.assertEqual(travel[0], 3.2)
        self.assertEqual(travel[7], 3.2)
        self.assertEqual(target, self.transport.profile)
        legacy = deepcopy(target)
        for key in legacy["keys"]:
            key.pop("bottom_deadzone")
        normalized, travel = parse_studio_profile(legacy)
        self.assertEqual(normalized, target)
        self.assertIsNone(travel)
        self.assertEqual(self.transport.mutations(), [])

    def test_whole_profile_capability_preflight_before_output_or_first_write(self):
        transport, _, session, tools = setup_tools(capabilities=0x1f)
        target = deepcopy(transport.profile)
        target["keys"][0]["actuation"] = 250
        target["bindings"][-1] = dict(input=27, kind=5, code=1, modifiers=0)
        with self.assertRaisesRegex(ProtocolError, "firmware"):
            tools["keyboard_import_studio_profile"](make_studio_profile(target, [3.] * 14))
        self.assertEqual(transport.mutations(), [])
        self.assertEqual(session.travel_mm, [4.] * 14)

    def test_studio_import_changes_metadata_only_on_complete_verified_apply(self):
        target = deepcopy(self.transport.profile)
        target["keys"][0]["actuation"] = 250
        target["bindings"][0] = dict(input=0, kind=1, code=0x90, modifiers=0)
        studio = make_studio_profile(target, [3.5] * 14)
        original_set_binding = self.client.set_binding
        self.client.set_binding = lambda record: (_ for _ in ()).throw(DeviceError("failed binding"))
        failed = self.tools["keyboard_import_studio_profile"](studio)
        self.assertFalse(failed["complete"])
        self.assertFalse(failed["ui_metadata_applied"])
        self.assertEqual(self.session.travel_mm, [4.] * 14)
        self.client.set_binding = original_set_binding
        success = self.tools["keyboard_import_studio_profile"](studio)
        self.assertTrue(success["complete"])
        self.assertTrue(success["ui_metadata_applied"])
        self.assertEqual(self.session.travel_mm, [3.5] * 14)
        self.assertNotIn(SAVE, self.transport.mutations())

    def test_invalid_ui_metadata_prevents_every_device_write(self):
        studio = make_studio_profile(self.transport.profile, [4.] * 14)
        studio["ui"]["totalTravelMm"][2] = float("nan")
        with self.assertRaises(ProtocolError):
            self.tools["keyboard_import_studio_profile"](studio)
        self.assertEqual(self.transport.mutations(), [])

    def test_read_only_blocks_all_config_mutations_and_ui_metadata_changes(self):
        transport, _, session, tools = setup_tools(write=False)
        calls = [("keyboard_set_travel_mm", ([0], 3.5)),
                 ("keyboard_set_keys", ([0], {"actuation": 250})),
                 ("keyboard_set_bindings", ([dict(input=1, kind=1, code=4, modifiers=0)],)),
                 ("keyboard_import_studio_profile", (deepcopy(transport.profile),)),
                 ("keyboard_defaults", ())]
        for name, args in calls:
            with self.subTest(name=name), self.assertRaises(PermissionError):
                tools[name](*args)
        self.assertEqual(transport.mutations(), [])
        self.assertEqual(session.travel_mm, [4.] * 14)

    def test_active_batch_or_firmware_calibration_blocks_config_changes(self):
        self.session.calibration = SimpleNamespace(active=True)
        with self.assertRaisesRegex(DeviceError, "batch calibration"):
            self.tools["keyboard_set_bindings"]([dict(input=1, kind=1, code=4, modifiers=0)])
        self.session.calibration = SimpleNamespace(active=False)
        self.transport.cal["state"] = 2
        with self.assertRaisesRegex(DeviceError, "individual calibration"):
            self.tools["keyboard_set_travel_mm"]([0], 3.5)
        self.assertEqual(self.transport.mutations(), [])

    def test_defaults_is_ram_only_output_off_and_retains_ui_travel(self):
        self.session.travel_mm = [3.5] * 14
        before = profile_fingerprint(self.transport.profile)
        result = self.tools["keyboard_defaults"](expected_fingerprint=before)
        self.assertTrue(result["reset_ram"])
        self.assertTrue(result["readback_verified"])
        self.assertEqual(result["profile"], defaults())
        self.assertFalse(result["persisted"])
        self.assertEqual(self.transport.mutations(), [OUTPUT, DEFAULTS])
        self.assertEqual(self.session.travel_mm, [3.5] * 14)
        self.assertEqual(self.transport.generation, 0)

    def test_readonly_layout_catalog_work_without_device(self):
        self.session.client = None
        self.session.require_client = lambda: (_ for _ in ()).throw(DeviceError("not connected"))
        self.assertEqual(self.tools["keyboard_layout"]()["input_count"], 28)
        catalog = self.tools["keyboard_binding_catalog"]()
        self.assertEqual(catalog["kinds"]["6"], "keyboard_chord")
        self.assertEqual(catalog["keyboard_chord"]["capability"], 0x2000)

    def test_defaults_lost_ack_reports_uncertain_result_without_retry(self):
        original = self.client.request
        def lose_defaults_reply(op, *args, **kwargs):
            result = original(op, *args, **kwargs)
            if op == DEFAULTS:
                raise TimeoutError("defaults reply lost")
            return result
        self.client.request = lose_defaults_reply
        result = self.tools["keyboard_defaults"]()
        self.assertIsNone(result["reset_ram"])
        self.assertFalse(result["readback_verified"])
        self.assertFalse(result["command_acknowledged"])
        self.assertEqual(result["profile"], defaults())
        self.assertIn("uncertain", result["error"])
        self.assertEqual(self.transport.mutations(), [OUTPUT, DEFAULTS])

    @unittest.skipUnless(shutil.which("node") and os.environ.get("SPLIT_BEAT_STUDIO"), "offline layout parity test requires Node and a Studio checkout")
    def test_layout_and_catalog_cover_actual_gui_without_invented_inputs(self):
        root = Path(os.environ["SPLIT_BEAT_STUDIO"])
        source = "import {INPUTS,CONTROLS} from './web/layout.mjs';import {KEY_OPTIONS} from './web/key-catalog.mjs';console.log(JSON.stringify({INPUTS,CONTROLS,KEY_OPTIONS}));"
        result = subprocess.run(["node", "--input-type=module", "-e", source], cwd=root, check=True, capture_output=True)
        gui = json.loads(result.stdout)
        records = layout()["inputs"]
        self.assertEqual(len(records), len(gui["INPUTS"]))
        for record, item in zip(records, gui["INPUTS"]):
            control = next(control for control in gui["CONTROLS"] if control["id"] == item["controlId"])
            for field in ("side", "type"):
                self.assertEqual(record[field], item[field])
            self.assertEqual(record["physical_label"], item["label"])
            self.assertEqual(record["input"], item["id"])
            self.assertEqual(record["source_ref"], control["sourceRef"])
            self.assertEqual(record["supports_rt"], item["type"] == "hall")
        catalog = binding_catalog()["named_keyboard_usages"]
        self.assertEqual({item["code"] for item in catalog}, {item["code"] for item in gui["KEY_OPTIONS"]})
        self.assertEqual(len(catalog), len({item["code"] for item in catalog}))


if __name__ == "__main__":
    unittest.main()
