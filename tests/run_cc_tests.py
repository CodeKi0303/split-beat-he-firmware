"""Run real FUSB303B policy with an asynchronous register model, no hardware."""
from pathlib import Path
import os,shutil,subprocess
root=Path(__file__).resolve().parents[1]
build=root/'build/host-cc';build.mkdir(parents=True,exist_ok=True)
sources=[root/'src/cc_fusb303b.c',root/'tests/test_cc.c']
if os.name=='nt':
    vcvars=Path(r'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
    command=['cl','/nologo','/std:c11','/W4','/WX','/O2',f'/I{root / "include"}',*map(str,sources),'/Fe:test_cc.exe']
    subprocess.run(f'call "{vcvars}" >nul && '+subprocess.list2cmdline(command),shell=True,cwd=build,check=True)
    exe=build/'test_cc.exe'
else:
    exe=build/'test_cc'
    subprocess.run([shutil.which('cc') or 'cc','-std=c11','-Wall','-Wextra','-Werror',f'-I{root / "include"}',*map(str,sources),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
