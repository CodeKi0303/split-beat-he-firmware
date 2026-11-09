"""Run: python -m unittest discover -s tests -p host_test.py -v"""
import copy
import struct
import time
import unittest

from host.protocol import *
from host.client import KeyboardClient, DeviceError
from host.simulator import DemoTransport, defaults


class HostTests(unittest.TestCase):
    def test_crc_known_vector(self):
        self.assertEqual(crc16(b'123456789'),0x29B1)

    def test_packet_validation(self):
        p=packet(INFO,2)
        self.assertEqual(unpack_packet(p),(INFO,2,0,b''))
        for index in (0,2,8,63):
            broken=bytearray(p);broken[index]^=1
            with self.assertRaises(ProtocolError): unpack_packet(broken)
        broken=bytearray(p);broken[10]=1;struct.pack_into('<H',broken,62,crc16(broken[:62]))
        with self.assertRaisesRegex(ProtocolError,'padding'): unpack_packet(broken)
        with self.assertRaises(ProtocolError): unpack_packet(p[:-1])
        with self.assertRaises(ProtocolError): packet(INFO,0)

    def test_profile_strict(self):
        p=defaults()
        self.assertEqual(validate_profile(p),p)
        cases=[]
        bad=copy.deepcopy(p);bad['keys'][0]['key']=1;cases.append(bad)
        bad=copy.deepcopy(p);bad['keys'][0]['flags']=7;cases.append(bad)
        bad=copy.deepcopy(p);bad['keys'][0]['actuation']=False;cases.append(bad)
        bad=copy.deepcopy(p);bad['bindings'][0].update(kind=5,code=6);cases.append(bad)
        bad=copy.deepcopy(p);bad['bindings'][0].update(kind=4,code=0xffff);cases.append(bad)
        bad=copy.deepcopy(p);bad['shell']='malicious';cases.append(bad)
        for bad in cases:
            with self.assertRaises(ProtocolError):validate_profile(bad)

    def test_record_roundtrips(self):
        p=defaults()
        for k in p['keys']:self.assertEqual(decode_key(encode_key(k)),k)
        for b in p['bindings']:self.assertEqual(decode_binding(encode_binding(b)),b)

    def test_chord_record_and_persisted_profile(self):
        two = dict(input=20, kind=6, keys=[4, 0xDF], modifiers=0x81)
        three = {**two, "keys": [4, 5, 0xDF]}
        for record, wire in ((two, bytes([20, 6, 4, 0xDF, 0x81, 0])),
                             (three, bytes([20, 6, 4, 5, 0x81, 0xDF]))):
            self.assertEqual(encode_binding(record), wire)
            self.assertEqual(decode_binding(wire), record)
        for keys in ([4], [4, 5, 6, 7], [4, 4], [5, 4], [0, 4], [4, 0xE0]):
            with self.assertRaises(ProtocolError):
                encode_binding({**two, "keys": keys})
        for wire in (bytes([20, 6, 4, 5, 0, 4]), bytes([20, 1, 4, 0, 0, 5])):
            with self.assertRaises(ProtocolError):
                decode_binding(wire)
        c = KeyboardClient(DemoTransport(), True)
        profile = defaults()
        profile["bindings"][20] = three
        self.assertFalse(c.import_profile(profile)["persisted"])
        self.assertTrue(c.save()["verified"])
        self.assertEqual(c.get_config(), profile)

    def test_bottom_record_legacy_and_limits(self):
        legacy=defaults()['keys'][0];del legacy['bottom_deadzone']
        self.assertEqual(key_record(legacy)['bottom_deadzone'],0)
        self.assertEqual(encode_key(legacy)[16:],bytes(6))
        modern={**legacy,'bottom_deadzone':125}
        self.assertEqual(decode_key(encode_key(modern)),modern)
        self.assertEqual(encode_key(modern)[16:18],bytes([125,0]))
        for value in (-1,801,False,None):
            with self.assertRaises(ProtocolError):key_record({**legacy,'bottom_deadzone':value})
        self.assertEqual(key_record({**legacy,'bottom_deadzone':800})['bottom_deadzone'],800)
        invalid=bytearray(encode_key(modern));invalid[20]=1
        with self.assertRaisesRegex(ProtocolError,'reserved'):decode_key(invalid)

    def test_bottom_capability_preflight_and_save(self):
        class Legacy033(DemoTransport):
            def __init__(self):super().__init__();self.mutations=[]
            def _command(self,op,p):
                if op in MUTATIONS:self.mutations.append(op)
                reply=super()._command(op,p)
                if op==INFO:
                    reply=bytearray(reply);reply[2]=3;struct.pack_into('<I',reply,16,0xff)
                return reply
        t=Legacy033();c=KeyboardClient(t,True);key=defaults()['keys'][0]
        with self.assertRaisesRegex(ProtocolError,'0.3.4'):c.set_key({**key,'bottom_deadzone':100})
        profile=defaults();profile['keys'][-1]['bottom_deadzone']=100
        with self.assertRaisesRegex(ProtocolError,'0.3.4'):c.import_profile(profile)
        self.assertEqual(t.mutations,[])
        self.assertEqual(c.set_key(key),key)
        self.assertEqual(t.mutations,[SET_KEY])
        modern=KeyboardClient(DemoTransport(),True)
        self.assertEqual(modern.info()['capabilities']&CAP_BOTTOM_DEADZONE,CAP_BOTTOM_DEADZONE)
        self.assertFalse(modern.import_profile(profile)['persisted'])
        self.assertTrue(modern.save()['verified'])
        self.assertEqual(modern.get_config(),profile)

    def test_info_hardware_revisions_and_layout(self):
        payload=bytearray(struct.pack('<8B5IH4B',0,1,0,0,14,6,4,28,16000,125,31,0,0,1024,77,1,2,0))
        for revision in (77,85,86):
            payload[30]=revision
            self.assertEqual(decode_info(payload)['board_rev'],revision)
        for offset,value in ((30,0),(30,84),(30,87),(30,255),(3,1),(4,13),(5,5),(6,3),(7,27),(31,2)):
            bad=payload.copy();bad[offset]=value
            with self.assertRaises(ProtocolError):decode_info(bad)

    def test_readonly_and_invalid_import_no_write(self):
        t=DemoTransport();c=KeyboardClient(t)
        self.assertTrue(c.info()['demo'])
        self.assertEqual(c.info()['scan_target_hz'],64000)
        with self.assertRaises(PermissionError):c.output(False)
        c.write_enabled=True
        bad=defaults();bad['bindings'][-1]['input']=0
        with self.assertRaises(ProtocolError):c.import_profile(bad)
        self.assertFalse(t.dirty)

    def test_configuration_client_cannot_dispatch_update_commands(self):
        class NoIO:
            calls = 0
            def write(self, data):
                self.calls += 1
                raise AssertionError("unsupported command reached the transport")
        for write_enabled in (False, True):
            t = NoIO()
            c = KeyboardClient(t, write_enabled)
            for op in [*range(0x30, 0x37), *range(0x50, 0x57), 0x3f, 0x4a, 0, 11, 255, True, 1.0]:
                with self.assertRaisesRegex(ProtocolError, "dedicated updater"):
                    c.request(op, b"UPD3")
            self.assertEqual(t.calls, 0)
            self.assertEqual(c.sequence, 0)

    def test_wrong_seq_and_malformed_ignored(self):
        class Inject(DemoTransport):
            def write(self,data):
                super().write(data)
                op,seq,_,p=unpack_packet(self.queue[0])
                self.queue.insert(0,packet(op,seq+1,p))
                self.queue.insert(0,b'bad')
        c=KeyboardClient(Inject())
        self.assertEqual(c.info()['board_rev'],86)

    def test_mouse_and_legacy_capability(self):
        class Legacy(DemoTransport):
            def __init__(self):super().__init__();self.mutations=0
            def _command(self,op,p):
                if op in MUTATIONS:self.mutations+=1
                reply=super()._command(op,p)
                if op==INFO:
                    reply=bytearray(reply);reply[1]=1;struct.pack_into('<I',reply,16,31)
                return reply
        t=Legacy();c=KeyboardClient(t,True)
        for binding in (dict(input=0,kind=5,code=1,modifiers=0),dict(input=0,kind=2,code=-1,modifiers=0)):
            with self.assertRaisesRegex(ProtocolError,'firmware 0.2'):c.set_binding(binding)
            profile=defaults();profile['bindings'][0]=binding
            with self.assertRaisesRegex(ProtocolError,'firmware 0.2'):c.import_profile(profile)
        self.assertEqual(t.mutations,0)
        chord = dict(input=0, kind=6, keys=[4, 5], modifiers=0)
        with self.assertRaisesRegex(ProtocolError, "0x2000"):
            c.set_binding(chord)
        profile = defaults()
        profile["bindings"][0] = chord
        with self.assertRaisesRegex(ProtocolError, "0x2000"):
            c.import_profile(profile)
        self.assertEqual(t.mutations,0)
        c=KeyboardClient(DemoTransport(),True)
        for inp in (0,14,20):
            for button in range(1,6):
                b=dict(input=inp,kind=5,code=button,modifiers=0)
                self.assertEqual(c.set_binding(b),b)
        self.assertEqual(c.info()['capabilities']&CAP_MOUSE_BUTTON,CAP_MOUSE_BUTTON)

    def test_timeout_never_retries_write(self):
        class Silence(DemoTransport):
            def __init__(self):super().__init__();self.writes=0
            def write(self,data):self.writes+=1
        t=Silence();c=KeyboardClient(t)
        with self.assertRaises(TimeoutError):c.request(INFO,timeout=.02)
        self.assertEqual(t.writes,1)

    def test_calibration_output_save(self):
        t=DemoTransport();c=KeyboardClient(t,True)
        with self.assertRaises(DeviceError):c.output(True)
        for i in range(14):
            c.calibrate(i,'capture_rest');time.sleep(.03)
            self.assertEqual(c.status(i//7)['calibration_state'],2)
            c.calibrate(i,'capture_press');time.sleep(.03)
            self.assertEqual(c.status(i//7)['calibration_state'],4)
            c.calibrate(i,'commit')
        self.assertTrue(c.output(True)['output_enabled'])
        expected=c.get_config()
        saved=c.save()
        self.assertTrue(saved['persisted'] and saved['verified'])
        self.assertEqual(c.get_config(),expected)
        self.assertTrue(saved['output_enabled'] and c.status()['output_enabled'])
        c.output(False)
        self.assertFalse(c.save()['output_enabled'])

    def test_save_ack_is_not_success(self):
        class FailedSave(DemoTransport):
            def reconnect(self,timeout=15):self.generation=0
        c=KeyboardClient(FailedSave(),True)
        with self.assertRaisesRegex(DeviceError,'readback FAILED'):c.save()

    def test_import_does_not_save(self):
        t=DemoTransport();c=KeyboardClient(t,True);p=defaults();p['bindings'][0]['code']=44
        self.assertFalse(c.import_profile(p)['persisted'])
        self.assertEqual(c.get_config(),p)
        self.assertEqual(t.generation,0)
        self.assertIsNone(t.stored)


if __name__=='__main__':unittest.main()
