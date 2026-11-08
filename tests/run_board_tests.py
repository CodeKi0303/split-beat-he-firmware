"""Compile and run pure board logic on the host; never opens hardware."""
from pathlib import Path
import os
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / 'build/host-board-tests'
build.mkdir(parents=True, exist_ok=True)
cc = next((shutil.which(x) for x in ('cl', 'clang', 'gcc') if shutil.which(x)), None)
vcvars = None
if cc is None and os.name == 'nt':
    candidate = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
    if candidate.exists():
        cc, vcvars = 'cl', candidate
if cc is None:
    raise SystemExit('Host C compiler required (MSVC, clang, or gcc).')
exe = build / ('test_board_logic.exe' if os.name == 'nt' else 'test_board_logic')
sources = [root / f'src/{x}.c' for x in ('input_scan', 'split_codec')] + [root / 'tests/test_board_logic.c']
if Path(cc).stem.lower() == 'cl':
    command = [cc, '/nologo', '/std:c11', '/W4', '/WX', '/O2', f'/I{root / "include"}', *map(str, sources), f'/Fe:{exe}']
else:
    command = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', f'-I{root / "include"}', *map(str, sources), '-o', str(exe)]
if vcvars:
    subprocess.run(f'call "{vcvars}" >nul && ' + subprocess.list2cmdline(command), shell=True, cwd=build, check=True)
else:
    subprocess.run(command, cwd=build, check=True)
subprocess.run([str(exe)], cwd=build, check=True)

# Compile the actual GPIO setup in isolation with a host HAL observer. This
# catches a driven-high FUSB EN_N, pull-ups, lost preload, and status pins
# accidentally inheriting the enable pin's output mode.
source=(root/'src/board.c').read_text(encoding='utf-8')
start=source.index('static void gpio_outputs(')
end=source.index('\nvoid board_init(',start)
(build/'safe_gpio_under_test.inc').write_text(source[start:end],encoding='utf-8')
for hardware in ('c77','c85','c86'):
    for left in (0,1):
        gpio_exe=build/f'test_safe_gpio_{hardware}_{left}{".exe" if os.name=="nt" else ""}'
        defines=[f'HARDWARE_REVISION={hardware[1:]}',f'ROLE_LEFT={left}']
        if Path(cc).stem.lower()=='cl':
            command=[cc,'/nologo','/std:c11','/W4','/WX','/O2',
                     f'/I{root/"include"}',f'/I{build}',*[f'/D{x}' for x in defines],
                     str(root/'tests/test_safe_gpio.c'),f'/Fe:{gpio_exe}']
        else:
            command=[cc,'-std=c11','-Wall','-Wextra','-Werror','-O2',
                     f'-I{root/"include"}',f'-I{build}',*[f'-D{x}' for x in defines],
                     str(root/'tests/test_safe_gpio.c'),'-o',str(gpio_exe)]
        if vcvars:
            subprocess.run(f'call "{vcvars}" >nul && '+subprocess.list2cmdline(command),shell=True,cwd=build,check=True)
        else:
            subprocess.run(command,cwd=build,check=True)
        subprocess.run([str(gpio_exe)],cwd=build,check=True)
