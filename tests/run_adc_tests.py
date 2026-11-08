"""Run the production ADC publication/IRQ logic on the host; no device access."""
from pathlib import Path
import os
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / 'build/host-adc-tests'
build.mkdir(parents=True, exist_ok=True)
cc = next((shutil.which(x) for x in ('cl', 'clang', 'gcc') if shutil.which(x)), None)
vcvars = None
if cc is None and os.name == 'nt':
    candidate = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
    if candidate.exists():
        cc, vcvars = 'cl', candidate
if cc is None:
    raise SystemExit('Host C compiler required (MSVC, clang, or gcc).')

source = (root / 'src/adc_scan.c').read_text(encoding='utf-8')
start = source.index('static void halt_from_irq(')
end = source.index('\nvoid adc_scan_stop(', start)
(build / 'adc_scan_under_test.inc').write_text(source[start:end], encoding='utf-8')
trace_start = source.index('\nbool adc_scan_trace_snapshot(') + 1
trace_end = source.index('\n#endif', trace_start)
(build / 'adc_trace_under_test.inc').write_text(source[trace_start:trace_end], encoding='utf-8')
dual_source = (root / 'src/adc_scan_dual96.inc').read_text(encoding='utf-8')
dual_start = dual_source.index('static void halt_from_irq(')
dual_end = dual_source.index('\nstatic int init_failed(', dual_start)
(build / 'adc_dual96_under_test.inc').write_text(dual_source[dual_start:dual_end], encoding='utf-8')

dual64_source = (root / 'src/adc_scan_dual64.inc').read_text(encoding='utf-8')
dual64_start = dual64_source.index('static void halt_from_irq(')
dual64_end = dual64_source.index('\nstatic int init_failed(', dual64_start)
(build / 'adc_dual64_under_test.inc').write_text(dual64_source[dual64_start:dual64_end], encoding='utf-8')
async_source = (root / 'src/adc_scan_async64.inc').read_text(encoding='utf-8')
async_end = async_source.index('\nstatic int init_failed(')
(build / 'adc_async64_under_test.inc').write_text(async_source[:async_end], encoding='utf-8')

for name, hardware, rate, adc_hz, recovery, left in (
    ('c77', 77, 16000, 12500000, False, True),
    ('c85', 85, 16000, 12500000, False, True),
    ('c86', 86, 64000, 40000000, False, True),
    ('c86-single75', 86, 75000, 40000000, False, True),
    ('c86-right', 86, 64000, 40000000, False, False),
    ('c86-recovery', 86, 16000, 12500000, True, True),
    ('c86-dual96', 86, 96000, 20000000, False, True),
    ('c86-dual64', 86, 64000, 30000000, False, True),
    ('c86-dual96-12bit', 86, 96000, 30000000, False, True),
    ('c86-single-diag', 86, 64000, 40000000, False, True),
    ('c86-dual40-diag', 86, 64000, 40000000, False, True),
    ('c86-async64', 86, 64000, 40000000, False, True),
    ('c86-async96', 86, 96000, 40000000, False, True),
    ('c86-async96-right', 86, 96000, 40000000, False, False),
):
    exe = build / f'test_adc_{name}{".exe" if os.name == "nt" else ""}'
    defines = [f'HARDWARE_REVISION={hardware}', f'EXPECTED_SCAN_HZ={rate}', f'EXPECTED_ADC_HZ={adc_hz}', f'ROLE_LEFT={int(left)}']
    if recovery or rate in (75000, 96000):
        defines += [f'C86_SCAN_TARGET_HZ={rate}']
    if name in ('c86-dual64', 'c86-dual40-diag', 'c86-async64'):
        defines += ['C86_ADC_DUAL64=1']
    if name.endswith('-diag'):
        defines += ['C86_ADC_CAPTURE_DIAG=1']
    if name == 'c86-dual40-diag':
        defines += ['C86_ADC_DUAL64_MATCHED40=1']
    if name == 'c86-dual96-12bit':
        defines += ['C86_ADC_DUAL96_12BIT=1']
    if name == 'c86-async64':
        defines += ['C86_ADC_ASYNC64=1']
    if name in ('c86-async96', 'c86-async96-right'):
        defines += ['C86_ADC_ASYNC96=1']
    test_source = root / ('tests/test_adc_async64.c' if name.startswith('c86-async') else
                         'tests/test_adc_dual64.c' if name in ('c86-dual64', 'c86-dual96-12bit', 'c86-dual40-diag') else
                         'tests/test_adc_dual96.c' if rate == 96000 else 'tests/test_adc_scan.c')
    sources = [str(test_source)]
    if name == 'c86-async96-right':
        sources.append(str(root / 'src/split_codec.c'))
    if Path(cc).stem.lower() == 'cl':
        command = [cc, '/nologo', '/std:c11', '/W4', '/WX', '/O2',
                   f'/I{root / "include"}', f'/I{build}', *[f'/D{x}' for x in defines],
                   *sources, f'/Fe:{exe}']
    else:
        command = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                   f'-I{root / "include"}', f'-I{build}', *[f'-D{x}' for x in defines],
                   *sources, '-o', str(exe)]
    if vcvars:
        subprocess.run(f'call "{vcvars}" >nul && ' + subprocess.list2cmdline(command),
                       shell=True, cwd=build, check=True)
    else:
        subprocess.run(command, cwd=build, check=True)
    subprocess.run([str(exe)], cwd=build, check=True)

# Compile-time selection must reject experimental profiles on the wrong target
# and invalid dual64 flags. These probes never link or touch USB.
probe = build / 'scan_profile_probe.c'
probe.write_text('#include "scan_profile.h"\n', encoding='utf-8')
def reject_profile(defines, marker):
    if Path(cc).stem.lower() == 'cl':
        command = [cc, '/nologo', '/EP', f'/I{root / "include"}',
                   *[f'/D{x}' for x in defines], str(probe)]
    else:
        command = [cc, '-std=c11', '-E', f'-I{root / "include"}',
                   *[f'-D{x}' for x in defines], str(probe)]
    if vcvars:
        result = subprocess.run(f'call "{vcvars}" >nul && ' + subprocess.list2cmdline(command),
                                shell=True, cwd=build, capture_output=True, text=True,
                                encoding='utf-8', errors='replace')
    else:
        result = subprocess.run(command, cwd=build, capture_output=True, text=True,
                                encoding='utf-8', errors='replace')
    if result.returncode == 0 or marker not in result.stdout + result.stderr:
        raise AssertionError(f'Forbidden scan profile was not rejected: {defines}\n{result.stdout}\n{result.stderr}')

for hardware, left, rate, flag, batched in (
    (77, 1, 64000, 1, 0), (85, 1, 64000, 1, 0), (86, 0, 64000, 1, 0),
    (86, 1, 16000, 1, 0), (86, 1, 96000, 1, 0), (86, 1, 64000, 2, 0),
    (77, 1, 75000, 0, 0), (86, 0, 75000, 0, 0),
    (77, 1, 96000, 0, 1), (85, 1, 96000, 0, 1), (86, 0, 96000, 0, 1),
    (86, 1, 64000, 0, 1), (86, 1, 16000, 0, 1), (86, 1, 96000, 1, 1),
    (86, 1, 96000, 0, 2),
):
    defines = [f'HARDWARE_REVISION={hardware}', f'ROLE_LEFT={left}',
               f'C86_SCAN_TARGET_HZ={rate}', f'C86_ADC_DUAL64={flag}',
               f'C86_ADC_DUAL96_12BIT={batched}']
    if batched:
        marker = 'C86_ADC_DUAL96_12BIT must' if batched == 2 else 'Batched dual96 12-bit ADC requires'
    elif rate == 75000:
        marker = 'The75k single-ADC candidate requires'
    else:
        marker = 'C86_ADC_DUAL64 must' if flag == 2 else 'Dual64 ADC requires'
    reject_profile(defines, marker)
for hardware, left, rate, dual, diag, matched, marker in (
    (77, 1, 64000, 0, 1, 0, 'ADC capture diagnostic requires'),
    (86, 0, 64000, 0, 1, 0, 'ADC capture diagnostic requires'),
    (86, 1, 75000, 0, 1, 0, 'ADC capture diagnostic requires'),
    (86, 1, 64000, 1, 1, 0, 'ADC capture diagnostic requires'),
    (86, 1, 64000, 0, 2, 0, 'C86_ADC_CAPTURE_DIAG must'),
    (86, 1, 64000, 1, 0, 1, 'Matched40 dual64 ADC requires'),
    (86, 1, 64000, 0, 1, 1, 'Matched40 dual64 ADC requires'),
    (86, 0, 64000, 1, 1, 1, 'Matched40 dual64 ADC requires'),
    (86, 1, 96000, 1, 1, 1, 'Matched40 dual64 ADC requires'),
    (86, 1, 64000, 1, 1, 2, 'C86_ADC_DUAL64_MATCHED40 must'),
):
    reject_profile([f'HARDWARE_REVISION={hardware}', f'ROLE_LEFT={left}',
                    f'C86_SCAN_TARGET_HZ={rate}', f'C86_ADC_DUAL64={dual}',
                    f'C86_ADC_CAPTURE_DIAG={diag}', f'C86_ADC_DUAL64_MATCHED40={matched}'], marker)
print('75k/dual64/dual96-12bit/diagnostic selection: unsupported hardware, roles, rates and flags rejected')
