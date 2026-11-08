"""Cross-language integration against REAL C core, not the Python simulator.

Root native build provides build/host/core_bridge.exe (or supply path).
Produces build/host/bridge-vectors.json for host_core_bridge.mjs.
"""
import json
import pathlib
import subprocess
import sys

ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from host.client import KeyboardClient
from host.protocol import *


class CoreTransport:
    demo=True
    def __init__(self,exe):
        self.process=subprocess.Popen([str(exe)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
        self.reply=None
        self.vectors=[]
    def write(self,data):
        self.process.stdin.write(data.hex()+'\n');self.process.stdin.flush()
        line=self.process.stdout.readline().strip()
        if not line:raise RuntimeError('C core terminated: '+self.process.stderr.read())
        self.reply=bytes.fromhex(line)
        self.vectors.append(dict(request=data.hex(),response=line))
    def read(self,timeout_ms):
        reply,self.reply=self.reply,None
        return reply or b''
    def reconnect(self,timeout=15):pass
    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=3)


def main():
    exe=pathlib.Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/'build/host/core_bridge.exe'
    if not exe.exists():raise SystemExit('Build core_bridge first: '+str(exe))
    t=CoreTransport(exe);c=KeyboardClient(t,True)
    try:
        info=c.info();assert info['board_rev']==77 and info['hall']==14
        profile=c.get_config();assert len(profile['keys'])==14 and len(profile['bindings'])==28
        assert info['capabilities'] & CAP_BOTTOM_DEADZONE
        key={**profile['keys'][0],'actuation':350,'rt_press':55,'rt_release':65,'bottom_deadzone':125}
        assert c.set_key(key)==key
        c.calibrate(0,'capture_rest')
        assert c.status()['calibration_state']==2
        c.calibrate(0,'capture_press')
        assert c.status()['calibration_state']==4
        c.calibrate(0,'commit')
        k=c.get_config()['keys'][0]
        assert k['flags']&4 and abs(k['released']-k['pressed'])>=128 and k['bottom_deadzone']==125
        binding=dict(input=20,kind=3,code=-1,modifiers=0)
        assert c.set_binding(binding)==binding
        assert c.get_config()['bindings'][20]==binding
        mouse=dict(input=0,kind=5,code=5,modifiers=0)
        assert c.set_binding(mouse)==mouse and c.get_config()['bindings'][0]==mouse
        before=c.info()['config_crc']
        saved=c.save()
        assert saved['verified'] and saved['config_crc']==before
        assert not c.status()['output_enabled']
        dest=ROOT/'build/host/bridge-vectors.json'
        dest.parent.mkdir(parents=True,exist_ok=True)
        dest.write_text(json.dumps(t.vectors,indent=2),encoding='utf8')
        print(f'C core -> Python client calibration/mapping/SAVE/readback PASS; {len(t.vectors)} golden exchanges')
    finally:t.close()


if __name__=='__main__':main()
