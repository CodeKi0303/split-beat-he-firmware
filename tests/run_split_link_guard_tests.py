"""Exercise actual split-link handlers on host mocks; never opens hardware."""
from pathlib import Path
import os
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / 'build/host-split-link-guard-tests'
build.mkdir(parents=True, exist_ok=True)
source = (root / 'src/split_link.c').read_text(encoding='utf-8')
sections = (
    ('#if !ROLE_LEFT\nstatic void arm_reply_guard(', '\nstatic void consume_rx('),
    ('void TIM4_IRQHandler(', '\nvoid split_link_start('),
    ('void split_link_stats(', None),
)
parts = []
for start_marker, end_marker in sections:
    start = source.index(start_marker)
    end = source.index(end_marker, start) if end_marker else len(source)
    parts.append(source[start:end])
(build / 'split_link_under_test.inc').write_text('\n'.join(parts), encoding='utf-8')
cc = next((shutil.which(name) for name in ('cl', 'clang', 'gcc') if shutil.which(name)), None)
vcvars = None
if cc is None and os.name == 'nt':
    candidate = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
    if candidate.exists():
        cc, vcvars = 'cl', candidate
if cc is None:
    raise SystemExit('Host C compiler required (MSVC, clang, or gcc).')
for left in (0, 1):
    exe = build / f'test_split_link_{left}{".exe" if os.name == "nt" else ""}'
    sources = [root / 'src/split_codec.c', root / 'tests/test_split_link_guard.c']
    if Path(cc).stem.lower() == 'cl':
        command = [cc, '/nologo', '/std:c11', '/W4', '/WX', '/O2',
                   f'/I{root / "include"}', f'/I{build}', f'/DROLE_LEFT={left}',
                   *map(str, sources), f'/Fe:{exe}']
    else:
        command = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                   f'-I{root / "include"}', f'-I{build}', f'-DROLE_LEFT={left}',
                   *map(str, sources), '-o', str(exe)]
    if vcvars:
        subprocess.run(f'call "{vcvars}" >nul && ' + subprocess.list2cmdline(command),
                       shell=True, cwd=build, check=True)
    else:
        subprocess.run(command, cwd=build, check=True)
    subprocess.run([str(exe)], cwd=build, check=True)
