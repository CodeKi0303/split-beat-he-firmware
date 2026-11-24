"""Profiles (command 0x4D) through the Python client and the device simulator. No USB."""
import unittest

from host.client import KeyboardClient
from host.protocol import (ProtocolError, PROFILE_NAME_BYTES, decode_profile_name, decode_profile_state,
                           encode_profile_name)
from host.simulator import DemoTransport


class ProfileCodec(unittest.TestCase):
    def test_names(self):
        self.assertEqual(encode_profile_name(""), bytes(PROFILE_NAME_BYTES))
        data = encode_profile_name("리듬")
        self.assertEqual(len(data), PROFILE_NAME_BYTES)
        self.assertEqual(decode_profile_name(data), "리듬")
        for bad in ("x" * 17, "가나다라마바", "a	b", 3):
            with self.assertRaises(ProtocolError):
                encode_profile_name(bad)
        with self.assertRaises(ProtocolError):
            decode_profile_name(b"A\x00B" + bytes(13))

    def test_state(self):
        self.assertEqual(decode_profile_state(bytes([4, 1, 0b0011])), dict(count=4, active=1, used=[0, 1]))
        for bad in (bytes([4, 2, 0b0011]), bytes([3, 0, 1]), bytes([4, 0, 0x10]), bytes([4, 0])):
            with self.assertRaises(ProtocolError):
                decode_profile_state(bad)


class ProfileClient(unittest.TestCase):
    def setUp(self):
        self.t = DemoTransport()
        self.c = KeyboardClient(self.t, write_enabled=True)

    def test_switch_keeps_each_profile(self):
        self.assertEqual(self.c.profiles(), dict(count=4, active=0, used=[0], names=["", "", "", ""]))
        self.c.create_profile(1, 0)
        self.c.rename_profile(1, "FPS")
        self.c.select_profile(1)
        key = self.c.get_config()["keys"][4]
        self.c.set_key(dict(key, actuation=800, rt_press=150))
        binding = dict(input=4, kind=1, code=0x0B, modifiers=0)
        self.c.set_binding(binding)
        self.c.select_profile(0)
        config = self.c.get_config()
        self.assertEqual(config["keys"][4]["actuation"], 2000)
        self.assertEqual(config["bindings"][4]["code"], 0x09)
        self.c.select_profile(1)
        config = self.c.get_config()
        self.assertEqual((config["keys"][4]["actuation"], config["keys"][4]["rt_press"]), (800, 150))
        self.assertEqual(config["bindings"][4], binding)
        self.assertEqual(self.c.profiles()["names"][1], "FPS")

    def test_save_persists_all_profiles(self):
        self.c.create_profile(2, 0)
        self.c.select_profile(2)
        self.c.set_key(dict(self.c.get_config()["keys"][0], actuation=1500))
        self.c.request(7)
        self.t.reconnect()
        self.assertEqual(self.c.profiles()["active"], 2)
        self.assertEqual(self.c.profiles()["used"], [0, 2])
        self.assertEqual(self.c.get_config()["keys"][0]["actuation"], 1500)
        self.c.select_profile(0)
        self.assertEqual(self.c.get_config()["keys"][0]["actuation"], 2000)

    def test_refusals(self):
        with self.assertRaises(Exception):
            self.c.select_profile(3)
        with self.assertRaises(Exception):
            self.c.delete_profile(0)
        with self.assertRaises(ProtocolError):
            self.c.select_profile(4)
        read_only = KeyboardClient(DemoTransport(), write_enabled=False)
        self.assertEqual(read_only.profiles()["active"], 0)
        with self.assertRaises(PermissionError):
            read_only.create_profile(1, 0)


if __name__ == "__main__":
    unittest.main()
