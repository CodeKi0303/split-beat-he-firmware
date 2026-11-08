"""Build real USB callback tests with a native C11 compiler; no device access."""
from pathlib import Path
import os
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / "build" / "host-usb"
build.mkdir(parents=True, exist_ok=True)
includes = [root / "tests/usb_stubs", root / "include", root / "third_party/usb/Core/Inc"]
source = root / "tests/test_usb.c"
if os.name == "nt":
    vcvars = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
    command = f'call "{vcvars}" >nul && cl /nologo /std:c11 /W4 /WX /O2 '
    command += " ".join(f'/I"{p}"' for p in includes)
    command += f' "{source}" /Fe:test_usb.exe /Fo:test_usb.obj'
    subprocess.run(command, shell=True, cwd=build, check=True)
    exe = build / "test_usb.exe"
else:
    exe = build / "test_usb"
    subprocess.run([shutil.which("cc") or "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    *[f"-I{p}" for p in includes], str(source), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
