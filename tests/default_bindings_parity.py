"""Compare actual serialized C defaults against the public mapping baseline."""
from pathlib import Path
import json
import struct
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tests.run_core_tests import compile_target


def main():
    baseline=json.loads((ROOT/'default-bindings.json').read_text(encoding='utf-8'))
    assert baseline['schema']=='split-beat-he-default-bindings-v1'
    bindings=baseline['bindings']
    assert len(bindings)==28 and [b['input'] for b in bindings]==list(range(28))
    assert all(set(b)=={'input','kind','code','modifiers'} for b in bindings)
    expected=b''.join(struct.pack('<BBHBB',b['input'],b['kind'],b['code']&0xffff,b['modifiers'],0) for b in bindings)
    exe=compile_target('default_bindings_vectors',
        [ROOT/'src/config.c',ROOT/'tests/default_bindings_vectors.c'],['HARDWARE_REVISION=86'])
    encoded=bytes.fromhex(subprocess.check_output([str(exe)],text=True).strip())
    assert len(encoded)==476 and encoded[308:]==expected, 'C serialized bindings differ from canonical JSON'
    # Mapping-only change: no sampled endpoints, enabled calibration flag,
    # new thresholds/deadzones, or device identity may enter factory defaults.
    expected_hall=b''.join(struct.pack('<BB10H',i,3,0,0,200,100,100,30,2,0,0,0) for i in range(14))
    assert encoded[:308]==expected_hall, 'Factory Hall defaults unexpectedly changed'
    report={'status':'PASS','binding_records':28,'binding_wire_bytes':168,
        'hall_defaults_unchanged':True,'calibration_copied':False,'physical_device_access':False}
    (ROOT/'build/host/default-bindings-parity.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('C defaults -> canonical mapping PASS: all28 serialized bindings; Hall defaults unchanged')


if __name__=='__main__':
    main()
