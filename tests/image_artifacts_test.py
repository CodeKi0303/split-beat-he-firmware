"""Factory/package layout rejection tests. Synthetic inputs never touch hardware."""
import base64
import hashlib
import struct
import unittest
import zlib
import subprocess
import sys
from pathlib import Path
from tools.firmware_artifacts import (APP_BASE, BOOT_BASE, METADATA_BASE, SLOT_END,
                                      MAX_IMAGE, VERSION, DISPLAY_VERSION, expected_identity,
                                      ihex_text, make_bundle, make_factory,
                                      metadata_record, validate_bundle, validate_image, validate_build_info)


def fixture(role='left', boot=False):
    data=bytearray(b'\xff'*0x800)
    base=BOOT_BASE if boot else APP_BASE
    struct.pack_into('<II',data,0,0x20020000,base+0x441)
    data[0x400:0x420]=expected_identity(role,boot)
    return bytes(data)


class Artifacts(unittest.TestCase):
    def test_96k_rejects_wrong_hardware_or_right_target_before_build(self):
        root=Path(__file__).resolve().parents[1]
        for tool in ('build.py','tools/verify_images.py','tools/package_release.py'):
            for hardware,role in (('c77','left'),('c85','left'),('c86','right'),('c86','both')):
                with self.subTest(tool=tool,hardware=hardware,role=role):
                    result=subprocess.run([sys.executable,tool,'--hardware',hardware,'--role',role,
                                           '--scan-hz','96000'],cwd=root,capture_output=True,text=True)
                    self.assertEqual(result.returncode,2)
                    self.assertTrue('only' in result.stderr, result.stderr)

    def test_build_metadata_matches_release_and_embedded_version(self):
        # Regression: v0.3.4 binaries previously passed verification while
        # build-info still said0.3.3. Every hardware/role now shares one label.
        for hardware in ('c77','c85','c86'):
            for role in ('left','right'):
                image=fixture(role)
                info=dict(hardware_profile=hardware,role=role,
                          version=DISPLAY_VERSION,version_number=VERSION)
                validate_build_info(info,image,hardware,role)
                for field,value in [('version','0.3.3'),('version_number',0x00030003),
                                    ('version_number',True),('version_number',None),
                                    ('role','right' if role=='left' else 'left'),
                                    ('hardware_profile','c85' if hardware=='c86' else 'c86')]:
                    with self.subTest(hardware=hardware,role=role,field=field,value=value):
                        with self.assertRaisesRegex(ValueError,'metadata'):
                            validate_build_info({**info,field:value},image,hardware,role)
                if hardware=='c86':
                    old_image=bytearray(image)
                    struct.pack_into('<I',old_image,0x40c,0x00030003)
                    with self.assertRaisesRegex(ValueError,'identity'):
                        validate_build_info(info,old_image,hardware,role)

    def test_metadata_and_factory_boundaries(self):
        for role in ('left','right'):
            image,boot=fixture(role),fixture(role,True)
            record=metadata_record(image,role)
            words=struct.unpack('<8I',record)
            self.assertEqual(words[:6],(0x33505546,len(image),zlib.crc32(image),VERSION,
                             86|(int(role=='right')<<8)|(1<<16),APP_BASE))
            self.assertEqual(words[6],zlib.crc32(record[:24]))
            self.assertEqual(words[7],0x33505546^0xffffffff)
            factory=make_factory(boot,image,role)
            self.assertEqual(len(factory),SLOT_END-BOOT_BASE)
            self.assertEqual(factory[:len(boot)],boot)
            self.assertEqual(factory[APP_BASE-BOOT_BASE:APP_BASE-BOOT_BASE+len(image)],image)
            self.assertEqual(factory[METADATA_BASE-BOOT_BASE:],record)
            self.assertEqual(set(factory[len(boot):APP_BASE-BOOT_BASE]),{255})
            self.assertEqual(set(factory[APP_BASE-BOOT_BASE+len(image):METADATA_BASE-BOOT_BASE]),{255})

    def test_manifest_matches_binary_and_rejects_tampering(self):
        image=fixture();bundle=make_bundle(image,'left')
        validate_bundle(bundle,image,'left')
        self.assertEqual(base64.b64decode(bundle['image_base64']),image)
        self.assertEqual(bundle['sha256'],hashlib.sha256(image).hexdigest())
        for field,value in [('role','right'),('image_base',BOOT_BASE),('crc32',0),('image_size',0),
                            ('version_number',1),('version','0.3.3'),('schema','unknown'),('image_base64','')]:
            invalid={**bundle,field:value}
            with self.assertRaises(ValueError):validate_bundle(invalid,image,'left')

    def test_roles_and_partition_limits(self):
        image=fixture()
        with self.assertRaises(ValueError):validate_image(image,'right')
        with self.assertRaises(ValueError):validate_image(image,'left',boot=True)
        with self.assertRaises(ValueError):validate_image(image+b'\xff','left')
        with self.assertRaises(ValueError):validate_image(image+b'\xff'*(MAX_IMAGE-len(image)+32),'left')
        with self.assertRaises(ValueError):validate_image(image[:0x400],'left')
        for offset,value in [(0,0x24000000),(0,0x2001ffff),(4,APP_BASE+0x440),(4,APP_BASE+len(image)+1),
                             (0x40c,1),(0x410,BOOT_BASE)]:
            invalid=bytearray(image);struct.pack_into('<I',invalid,offset,value)
            with self.assertRaises(ValueError):validate_image(invalid,'left')

    def test_sparse_hex_contains_only_images_and_commit(self):
        image,boot=fixture(),fixture(boot=True)
        segments=[(BOOT_BASE,boot),(APP_BASE,image),(METADATA_BASE,metadata_record(image,'left'))]
        text=ihex_text(segments);memory={};base=0
        for line in text.splitlines():
            raw=bytes.fromhex(line[1:]);self.assertEqual(sum(raw)&255,0)
            size=raw[0];address=int.from_bytes(raw[1:3],'big');kind=raw[3];payload=raw[4:-1]
            self.assertEqual(size,len(payload))
            if kind==4:base=int.from_bytes(payload,'big')<<16
            elif kind==0:memory.update({base+address+i:value for i,value in enumerate(payload)})
        expected={origin+i:value for origin,data in segments for i,value in enumerate(data)}
        self.assertEqual(memory,expected)
        self.assertLess(max(memory),0x08060000)
        with self.assertRaises(ValueError):ihex_text([(APP_BASE,image),(APP_BASE+32,image)])


if __name__=='__main__':unittest.main()
