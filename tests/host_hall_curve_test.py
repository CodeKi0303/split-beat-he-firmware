"""Curve/profile/diagnostic contracts; all transports are offline simulators."""
from copy import deepcopy
import unittest

from host.client import KeyboardClient
from host.hall_curve import (curve_catalog, normalized_from_raw, normalized_to_mm,
                             raw_to_estimated_mm, recognized_travel)
from host.mcp_configuration import make_studio_profile, parse_studio_profile
from host.mcp_diagnostics import capture_key_timing, capture_sensors, capability_summary
from host.protocol import (CAP_HALL_CURVE, ProtocolError, decode_key, encode_key,
                           key_record, MUTATIONS, OUTPUT, SET_KEY)
from host.simulator import defaults, DemoTransport
from tests.host_mcp_configuration_test import setup_tools
from tests.host_key_timing_test import Client
from tests.host_session_diagnostics_test import Clock, SensorClient


class HallCurveTests(unittest.TestCase):
    def test_recognized_deadzone_boundaries_do_not_rescale_middle_or_raw_estimate(self):
        config = dict(top_deadzone=88, bottom_deadzone=88, flags=7,
                      released=1000, pressed=3000, curve_id=0)
        cases = {0: 0, 50: 0, 88: 0, 89: 89, 500: 500,
                 911: 911, 912: 1000, 950: 1000, 1000: 1000}
        for flags in (4, 5, 7, 15):
            for travel, expected in cases.items():
                key = {**config, "flags": flags}
                self.assertEqual(recognized_travel(travel, key), expected)
                self.assertAlmostEqual(normalized_to_mm(expected, 3.4), expected * .0034)
        for travel in range(1001):
            self.assertEqual(recognized_travel(travel, {}), travel)
        # Raw diagnostics retain the real small signal inside either deadzone.
        self.assertAlmostEqual(raw_to_estimated_mm(1100, config, 3.4), .17)
        self.assertAlmostEqual(raw_to_estimated_mm(2900, config, 3.4), 3.23)

    def test_demo_serializes_recognized_travel_without_changing_rt_internal_state(self):
        transport = DemoTransport()
        key = transport.profile["keys"][0]
        key.update(flags=7, released=1000, pressed=3000, top_deadzone=88,
                   bottom_deadzone=88, curve_id=0, noise_deadband=0)
        client = KeyboardClient(transport)
        for travel, expected in ((50, 0), (88, 0), (89, 89), (500, 500),
                                 (911, 911), (912, 1000), (950, 1000), (0, 0)):
            transport.pose = [travel] + [0] * 13
            status = client.status(0)
            self.assertEqual(status["raw"][0], 1000 + 2 * travel)
            self.assertEqual(status["travel"][0], expected)
            self.assertEqual(transport.models[0].previous_travel, travel)

    def test_old_records_import_as_linear_with_optional_bottom_independent(self):
        key = defaults()["keys"][0]
        for omitted in (("curve_id",), ("bottom_deadzone",), ("curve_id", "bottom_deadzone")):
            legacy = {k: v for k, v in key.items() if k not in omitted}
            self.assertEqual(key_record(legacy), key)
            self.assertEqual(encode_key(legacy)[16:], bytes(6))
        for value in (True, -1, 2, 1.0, None):
            with self.assertRaises(ProtocolError):
                key_record({**key, "curve_id": value})
        encoded = encode_key({**key, "curve_id": 1})
        self.assertEqual(encoded[18:22], b"\x01\x00\x00\x00")
        self.assertEqual(decode_key(encoded)["curve_id"], 1)
        bad = bytearray(encoded); bad[20] = 1
        with self.assertRaisesRegex(ProtocolError, "reserved"):
            decode_key(bad)

    def test_curve_nodes_and_proportional_stroke_do_not_claim_actual_caliper_mm(self):
        # 22.44% of the ADC span corresponds to 15000/33000 of travel.
        self.assertEqual(normalized_from_raw(2244, 0, 10000, 1), 455)
        self.assertEqual(normalized_from_raw(7756, 10000, 0, 1), 455)
        self.assertAlmostEqual(normalized_to_mm(455, 3.4), 1.547)
        self.assertAlmostEqual(normalized_to_mm(455, 4.0), 1.82)
        self.assertAlmostEqual(normalized_to_mm(455, 4.0) / normalized_to_mm(455, 3.4), 4 / 3.4)
        profile = {"flags": 4, "released": 0, "pressed": 10000, "curve_id": 1}
        self.assertAlmostEqual(raw_to_estimated_mm(2244, profile, 3.4), 1.547)
        self.assertIsNone(raw_to_estimated_mm(2244, {**profile, "flags": 0}, 3.4))

    def test_default_stroke_unchanged_and_catalog_has_provenance(self):
        transport, _, session, tools = setup_tools()
        transport.commands.clear()
        catalog = tools["keyboard_curve_catalog"]()
        self.assertEqual(catalog, curve_catalog())
        self.assertEqual(catalog["curves"][1]["recommended_total_travel_mm"], 3.4)
        self.assertEqual(catalog["curves"][1]["source_board"], "NeonHE Tab60 HE / OW Ti")
        self.assertIn("not a measured C86", catalog["limits"])
        self.assertEqual(session.travel_mm, [4.0] * 14)
        self.assertFalse(transport.commands)

    def test_old_firmware_rejects_curve_before_any_mutation_for_all_entrypoints(self):
        transport, client, _, tools = setup_tools(capabilities=0x1ff)
        target = deepcopy(transport.profile)
        target["keys"][-1]["curve_id"] = 1
        operations = (
            lambda: tools["keyboard_preview_keys"]([0, 7], {"curve_id": 1}),
            lambda: tools["keyboard_set_keys"]([0, 7], {"curve_id": 1}),
            lambda: client.set_key({**target["keys"][0], "curve_id": 1}),
            lambda: client.import_profile(target),
            lambda: tools["keyboard_import_studio_profile"](make_studio_profile(target, [3.4] * 14)),
        )
        for operation in operations:
            with self.assertRaisesRegex(ProtocolError, "0.3.5"):
                operation()
        self.assertEqual(transport.mutations(), [])

    def test_group_selection_preserves_every_endpoint_and_other_setting(self):
        transport, _, session, tools = setup_tools(capabilities=0x3ff)
        original = deepcopy(transport.profile)
        tools["keyboard_set_travel_mm"]([0, 7], 3.4)
        preview = tools["keyboard_preview_keys"]([0, 7], {"curve_id": 1})
        self.assertEqual(transport.mutations(), [])
        result = tools["keyboard_set_keys"]([0, 7], {"curve_id": 1}, expected_fingerprint=preview["fingerprint"])
        expected = deepcopy(original)
        expected["keys"][0]["curve_id"] = expected["keys"][7]["curve_id"] = 1
        self.assertTrue(result["complete"])
        self.assertFalse(result["persisted"])
        self.assertEqual(result["profile"], expected)
        self.assertEqual(transport.mutations(), [OUTPUT, SET_KEY, SET_KEY])
        studio = tools["keyboard_export_studio_profile"]()
        decoded, travel = parse_studio_profile(studio)
        self.assertEqual(decoded, expected)
        self.assertEqual(travel, session.travel_mm)
        self.assertEqual(travel[0], 3.4)
        self.assertEqual(travel[1], 4.0)

    def test_status_distance_is_scaled_once_while_raw_estimate_uses_curve(self):
        clock = Clock()
        client = Client(clock)
        config = defaults()
        config["keys"][9].update(flags=7, released=1000, pressed=3000, curve_id=1)
        client.get_config = lambda: config
        result = capture_key_timing(client, 9, .2, 100, total_travel_mm=3.4,
                                    clock=clock.clock, delay=clock.delay)
        down = next(row for row in result["samples"] if row["travel"] == 500)
        self.assertEqual(down["travel_mm"], 1.7)  # STATUS is already corrected
        self.assertGreater(down["raw_estimated_mm"], down["travel_mm"])
        self.assertEqual(result["curve_id"], 1)
        self.assertTrue(result["timing_usable"])
        info = dict(major=0, minor=3, patch=5, capabilities=CAP_HALL_CURVE)
        self.assertTrue(capability_summary(info)["nonlinear_hall_curve"])

    def test_sensor_distance_metadata_and_changed_configuration_rejection(self):
        clock = Clock()
        client = SensorClient(clock)
        config = defaults()
        for key in config["keys"]:
            key.update(flags=7, released=900, pressed=3000, curve_id=1)
        client.get_config = lambda: config
        client.info = lambda: dict(board_rev=86, generation=1, config_crc=123)
        result = capture_sensors(client, .2, 10, True, total_travel_mm=[3.4] * 14,
                                  clock=clock.clock, delay=clock.delay)
        self.assertEqual(result["curve_ids"], [1] * 14)
        self.assertEqual(result["samples"][0]["travel_mm"], [0.0] * 14)
        self.assertGreater(result["samples"][0]["raw_estimated_mm"][0], 0)
        calls = [0]
        def changed_info():
            calls[0] += 1
            return dict(board_rev=86, generation=1, config_crc=calls[0])
        client.info = changed_info
        with self.assertRaisesRegex(Exception, "Configuration changed"):
            capture_sensors(client, .2, 10, total_travel_mm=[3.4] * 14,
                            clock=clock.clock, delay=clock.delay)


if __name__ == "__main__":
    unittest.main()
