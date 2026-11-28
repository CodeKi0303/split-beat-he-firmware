"""Compatibility and release-edge checks for the 0x4E fixed-release key record."""
import unittest

from host.client import KeyboardClient
from host.mcp_configuration import propose_keys
from host.protocol import CAP_FINE_TRAVEL, CAP_FIXED_RELEASE, EXTENDED_KEY, SET_KEY, ProtocolError, encode_key
from host.sampled_rt import SampledRT
from host.simulator import DemoTransport, defaults


def calibrated():
    return {**defaults()["keys"][0], "flags": 5, "released": 1000, "pressed": 3000,
            "actuation": 2500, "top_deadzone": 200, "noise_deadband": 0}


class FixedReleaseTest(unittest.TestCase):
    def test_edge_and_legacy_equality(self):
        # Each ADC count represents five normalized units.
        separated = SampledRT({**calibrated(), "fixed_release": 1500})
        self.assertEqual([separated.step(v) for v in (1499, 1500, 1360, 1300, 1360, 1500)],
                         [False, True, True, False, False, True])
        linked = SampledRT(calibrated())
        self.assertEqual([linked.step(v) for v in (1500, 1500, 1499)], [True, True, False])

    def test_extended_read_write_and_legacy_preservation(self):
        transport = DemoTransport(fixed_release_supported=True)
        client = KeyboardClient(transport, write_enabled=True)
        record = {**calibrated(), "fixed_release": 1800}
        self.assertEqual(client.set_key(record), record)
        client.request(SET_KEY, encode_key({**calibrated(), "noise_deadband": 8}))
        self.assertEqual(client.get_config()["keys"][0], {**record, "noise_deadband": 8})
        self.assertTrue(client.save()["verified"])
        reader = KeyboardClient(transport)
        self.assertEqual(reader.get_config()["keys"][0]["fixed_release"], 1800)
        with self.assertRaises(PermissionError):
            reader.request(EXTENDED_KEY, encode_key(record, extended=True))
        client.set_key({**record, "fixed_release": 0})
        self.assertNotIn("fixed_release", reader.get_config()["keys"][0])

    def test_capability_and_group_validation(self):
        client = KeyboardClient(DemoTransport(), write_enabled=True)
        with self.assertRaisesRegex(ProtocolError, "0x02000000"):
            client.set_key({**calibrated(), "fixed_release": 1700})
        config = client.get_config()
        config["keys"][:2] = [{**calibrated(), "key": i} for i in range(2)]
        caps = CAP_FINE_TRAVEL | CAP_FIXED_RELEASE
        proposal = propose_keys(config, [0, 1], {"fixed_release": .7}, unit="mm",
                                travel_mm=[4.] * 14, capabilities=caps)
        self.assertEqual([key["fixed_release"] for key in proposal], [1750, 1750])
        config["keys"][1]["top_deadzone"] = 1800
        with self.assertRaisesRegex(ProtocolError, "fixed_release"):
            propose_keys(config, [0, 1], {"fixed_release": 1750}, capabilities=caps)
