"""Offline selected-board firmware build and C86 USB-update package. No device access."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from tools.source_evidence import source_evidence
from tools.firmware_artifacts import (APP_BASE, BOOT_BASE, METADATA_BASE, MAX_IMAGE,
                                      VERSION, DISPLAY_VERSION, validate_build_info,
                                      ihex_text, make_bundle, make_factory,
                                      metadata_record, validate_image)

ROOT = Path(__file__).resolve().parent
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')
if hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8')


def run(args):
    result = subprocess.run(list(map(str, args)), cwd=ROOT, capture_output=True, text=True,
                            encoding='mbcs' if os.name == 'nt' else 'utf-8', errors='replace')
    if result.returncode:
        print(result.stdout + result.stderr)
        raise RuntimeError(f'Command failed: {args[0]}')
    if result.stderr:
        print(result.stderr, end='')
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role', choices=['left', 'right', 'both'], default='both')
    parser.add_argument('--hardware', choices=['c77', 'c85', 'c86'], default='c77')
    parser.add_argument('--scan-hz', type=int, choices=[16000, 64000, 75000, 96000],
                        help='C86 default64k;16k recovery;75k left-only;96k supports both roles with --adc-async96')
    parser.add_argument('--adc-dual64', action='store_true',
                        help='Explicit C86 left64k12-bit ADC1/2 candidate; separate output directory')
    parser.add_argument('--adc-dual96-12bit', action='store_true',
                        help='Explicit C86 left96k12-bit ADC1/2 candidate; two scans per IRQ')
    parser.add_argument('--adc-capture-diag', action='store_true',
                        help='C86 left64k diagnostic capture; isolated app-only comparison build')
    parser.add_argument('--adc-dual64-matched40', action='store_true',
                        help='Pair with --adc-capture-diag for dual64 at the single-ADC40MHz clock')
    parser.add_argument('--adc-isr-probe', action='store_true',
                        help='Pair with --adc-capture-diag for narrow ADC IRQ timing instead of CPU profiling')
    parser.add_argument('--adc-async64', action='store_true',
                        help='C86 left64k independent ADC1/2 banks; incompatible with other ADC mode flags')
    parser.add_argument('--adc-async96', action='store_true',
                        help='C86 96k independent ADC1/2 banks; incompatible with other ADC mode flags')
    parser.add_argument('--toolchain', type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.scan_hz in (64000, 75000, 96000) and args.hardware != 'c86':
        parser.error('High-rate scanning is only supported for C86')
    if args.scan_hz == 75000 and args.role != 'left':
        parser.error('75kHz is a left-only experiment; specify --role left')
    if args.scan_hz == 96000 and args.role != 'left' and not args.adc_async96:
        parser.error('96kHz is a left-only experiment without --adc-async96; specify --role left')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    scan_hz = args.scan_hz or (64000 if args.hardware == 'c86' else 16000)
    if args.adc_async64 and (args.hardware != 'c86' or args.role != 'left' or args.scan_hz != 64000 or
                             args.adc_dual64 or args.adc_dual96_12bit or args.adc_capture_diag or
                             args.adc_dual64_matched40 or args.adc_isr_probe or args.adc_async96):
        parser.error('--adc-async64 requires --hardware c86 --role left --scan-hz 64000 and no other ADC mode flags')
    if args.adc_async96 and (args.hardware != 'c86' or args.scan_hz != 96000 or
                             args.adc_dual64 or args.adc_dual96_12bit or args.adc_capture_diag or
                             args.adc_dual64_matched40 or args.adc_isr_probe or args.adc_async64):
        parser.error('--adc-async96 requires --hardware c86 --scan-hz 96000 and no other ADC mode flags')
    if args.adc_isr_probe and not args.adc_capture_diag:
        parser.error('--adc-isr-probe requires --adc-capture-diag')
    if args.adc_capture_diag and (args.hardware != 'c86' or args.role != 'left' or scan_hz != 64000 or args.adc_dual64 or args.adc_dual96_12bit):
        parser.error('--adc-capture-diag requires C86 left64000Hz without --adc-dual64 or --adc-dual96-12bit')
    if args.adc_dual64_matched40 and not args.adc_capture_diag:
        parser.error('--adc-dual64-matched40 requires --adc-capture-diag')
    if args.adc_dual96_12bit and (args.hardware != 'c86' or args.role != 'left' or scan_hz != 96000 or args.adc_dual64):
        parser.error('--adc-dual96-12bit requires --hardware c86 --role left --scan-hz 96000 without --adc-dual64')
    if args.adc_dual64 and (args.hardware != 'c86' or args.role != 'left' or scan_hz != 64000):
        parser.error('--adc-dual64 requires --hardware c86 --role left and 64000 Hz')
    async_adc = args.adc_async64 or args.adc_async96
    dual96_14bit = scan_hz == 96000 and not args.adc_dual96_12bit and not args.adc_async96
    dual64 = args.adc_dual64 or args.adc_dual64_matched40 or args.adc_async64
    dual12 = dual64 or args.adc_dual96_12bit
    toolchain = args.toolchain or os.environ.get('ARM_GCC_BIN')
    if toolchain:
        toolchain = Path(toolchain).resolve()
    elif shutil.which('arm-none-eabi-gcc'):
        toolchain = Path(shutil.which('arm-none-eabi-gcc')).parent
    else:
        toolchain = ROOT.parent / '.tools/xpack-arm-none-eabi-gcc-15.2.1-1.1/bin'
        if not toolchain.is_dir():
            parser.error('Supply --toolchain or ARM_GCC_BIN / PATH')
    extension = '.exe' if os.name == 'nt' else ''
    gcc = toolchain / f'arm-none-eabi-gcc{extension}'
    objcopy = toolchain / f'arm-none-eabi-objcopy{extension}'
    manifest = json.loads((ROOT / 'third_party/manifest.json').read_text())
    for item in manifest['files']:
        if hashlib.sha256((ROOT / item['path']).read_bytes()).hexdigest() != item['sha256']:
            raise RuntimeError(f'Dependency changed: {item["path"]}')
    evidence = {}
    profile = None
    if args.hardware in ('c85', 'c86'):
        profile = json.loads((ROOT / f'hardware/{args.hardware}-profile.json').read_text(encoding='utf-8'))
        pins = ROOT / 'hardware' / profile['pin_manifest']
        assert hashlib.sha256(pins.read_bytes()).hexdigest() == profile['pin_manifest_sha256']
        pin_data = json.loads(pins.read_text(encoding='utf-8'))
        assert pin_data['source_files'][profile['native_board_path']] == profile['native_board_sha256']
        evidence = {f'hardware/{name}': hashlib.sha256((ROOT / 'hardware' / name).read_bytes()).hexdigest()
                    for name in [f'{args.hardware}-profile.json', profile['pin_manifest']]}
    input_evidence = source_evidence()
    architecture = ['-mcpu=cortex-m7', '-mthumb', '-mfpu=fpv5-d16', '-mfloat-abi=hard']
    includes = [ROOT / path for path in ('include', 'third_party/hal/Inc', 'third_party/cmsis/Include',
                                        'third_party/device/Include', 'third_party/usb/Core/Inc')]
    multilib = list((toolchain.parent / 'lib/gcc/arm-none-eabi').glob('*/thumb/v7e-m+dp/hard/libgcc.a'))
    libraries = ['-L', toolchain.parent / 'arm-none-eabi/lib/thumb/v7e-m+dp/hard']
    if len(multilib) == 1:
        libraries += ['-L', multilib[0].parent]
    compiler_version = run([gcc, '--version']).splitlines()[0]
    roles = ['left', 'right'] if args.role == 'both' else [args.role]
    for role in roles:
        base = ROOT / 'build' if args.hardware == 'c77' else ROOT / 'build' / args.hardware
        if args.hardware == 'c86' and scan_hz == 16000:
            base /= 'scan16k'
        elif scan_hz == 75000:
            base /= 'scan75k'
        elif args.adc_dual96_12bit:
            base /= 'scan96k-dual12'
        elif args.adc_async96:
            base /= 'scan96k-async'
        elif scan_hz == 96000:
            base /= 'scan96k'
        elif args.adc_dual64:
            base /= 'scan64k-dual'
        elif args.adc_async64:
            base /= 'scan64k-async'
        elif args.adc_isr_probe:
            base /= 'scan64k-dual40-isr-probe' if args.adc_dual64_matched40 else 'scan64k-single-isr-probe'
        elif args.adc_capture_diag:
            base /= 'scan64k-dual40-diag' if args.adc_dual64_matched40 else 'scan64k-single-diag'
        build = base / role
        build.mkdir(parents=True, exist_ok=True)
        flags = architecture + ['-Os', '-g3', '-ffunction-sections', '-fdata-sections', '-fno-common',
                                '-DSTM32H723xx', '-DUSE_HAL_DRIVER', '-DHSE_VALUE=25000000U',
                                f'-DROLE_LEFT={int(role == "left")}',
                                f'-DHARDWARE_REVISION={args.hardware[1:]}', '-Wall', '-Wextra']
        if args.hardware == 'c86':
            flags += [f'-DC86_SCAN_TARGET_HZ={scan_hz}U']
        if dual64:
            flags += ['-DC86_ADC_DUAL64=1']
        if args.adc_async64:
            flags += ['-DC86_ADC_ASYNC64=1']
        if args.adc_async96:
            flags += ['-DC86_ADC_ASYNC96=1']
        if args.adc_dual64_matched40:
            flags += ['-DC86_ADC_DUAL64_MATCHED40=1']
        if args.adc_capture_diag:
            flags += ['-DC86_ADC_CAPTURE_DIAG=1']
        if args.adc_dual96_12bit:
            flags += ['-DC86_ADC_DUAL96_12BIT=1']
        for include in includes:
            flags += ['-I', str(include)]

        def compile_image(boot_mode=False):
            objects_dir = build / ('boot-objects' if boot_mode else 'app-objects')
            objects_dir.mkdir(parents=True, exist_ok=True)
            excluded = {'boot.c', 'main.c' if boot_mode else 'boot_main.c'}
            if args.hardware != 'c86':
                excluded.update({'fw_runtime.c', 'fw_boot_cookie.c', 'fw_update.c',
                                 'fw_update_flash.c', 'fw_identity.c'})
            sources = [path for path in sorted((ROOT / 'src').glob('*.c')) if path.name not in excluded]
            if role == 'right':
                sources = [path for path in sources if path.name not in
                           ('usb_device.c', 'usb_descriptors.c', 'usbd_conf.c')]
            sources += sorted((ROOT / 'third_party/hal/Src').glob('*.c'))
            if role == 'left':
                sources += sorted((ROOT / 'third_party/usb/Core/Src').glob('*.c'))
            sources += [ROOT / 'third_party/device/system_stm32h7xx.c',
                        ROOT / 'third_party/device/startup_stm32h723xx.s']

            def compile_one(source):
                digest = hashlib.sha256(source.relative_to(ROOT).as_posix().encode()).hexdigest()[:8]
                obj = objects_dir / f'{source.stem}-{digest}.o'
                options = flags + [f'-DFW_UPDATE_BOOT={int(boot_mode)}']
                # Profiling is an opt-in application diagnostic, never part of
                # the permanent updater even when building a diagnostic app.
                if args.adc_capture_diag and not boot_mode:
                    options += ['-DC86_ADC_IRQ_PROBE=1'] if args.adc_isr_probe else ['-DC86_CPU_PROFILE=1']
                options += ['-std=c11'] if source.suffix == '.c' else ['-x', 'assembler-with-cpp']
                if source.parent == ROOT / 'src':
                    options += ['-Werror']
                run([gcc, *options, '-c', source, '-o', obj])
                return obj

            with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                objects = list(pool.map(compile_one, sources))
            elf = build / ('boot.elf' if boot_mode else f'{args.hardware}-{role}.elf')
            linker = 'boot_update.ld' if boot_mode else ('app_update.ld' if args.hardware == 'c86' else 'app.ld')
            map_name = 'boot.map' if boot_mode else 'app.map'
            print(run([gcc, *architecture, '-nostartfiles', '--specs=nano.specs', '--specs=nosys.specs',
                       *libraries, '-T', ROOT / 'linker' / linker, '-Wl,--gc-sections',
                       '-Wl,--print-memory-usage', f'-Wl,-Map={build / map_name}', *objects,
                       '-Wl,--start-group', '-lc', '-lm', '-Wl,--end-group', '-o', elf]), end='')
            return elf

        elf = compile_image()
        app_path = build / f'{args.hardware}-{role}-app.bin'
        run([objcopy, '-O', 'binary', elf, app_path])
        app = app_path.read_bytes()
        if args.hardware == 'c86':
            app += b'\xff' * (-len(app) % 32)
            validate_image(app, role)
            boot_elf = compile_image(boot_mode=True)
            run([objcopy, '-O', 'binary', boot_elf, build / 'boot.bin'])
            boot = (build / 'boot.bin').read_bytes()
            factory = make_factory(boot, app, role)
            record = metadata_record(app, role)
            app_path.write_bytes(app)
            (build / f'{args.hardware}-{role}-app.hex').write_text(ihex_text([(APP_BASE, app)]), encoding='ascii')
            (build / 'boot.hex').write_text(ihex_text([(BOOT_BASE, boot)]), encoding='ascii')
            (build / f'{args.hardware}-{role}-factory.hex').write_text(
                ihex_text([(BOOT_BASE, boot), (APP_BASE, app), (METADATA_BASE, record)]), encoding='ascii')
            (build / f'{args.hardware}-{role}-app.fw.json').write_text(
                json.dumps(make_bundle(app, role), indent=2) + '\n', encoding='utf-8')
        else:
            run([objcopy, '-O', 'ihex', elf, build / f'{args.hardware}-{role}-app.hex'])
            boot_elf = build / 'boot.elf'
            run([gcc, *architecture, '-Os', '-ffreestanding', '-fno-unwind-tables',
                 '-fno-asynchronous-unwind-tables', '-nostdlib', '-T', ROOT / 'linker/boot.ld',
                 ROOT / 'src/boot.c', '-o', boot_elf])
            run([objcopy, '-O', 'binary', boot_elf, build / 'boot.bin'])
            run([objcopy, '-O', 'ihex', boot_elf, build / 'boot.hex'])
            boot = (build / 'boot.bin').read_bytes()
            if len(boot) > 131072 or len(app) > 655360:
                raise RuntimeError('Flash partition overflow')
            lines = []
            for path in (build / 'boot.hex', build / f'{args.hardware}-{role}-app.hex'):
                lines += [line for line in path.read_text().splitlines() if line != ':00000001FF']
            (build / f'{args.hardware}-{role}-factory.hex').write_text(
                '\n'.join(lines + [':00000001FF']) + '\n', encoding='ascii')
            factory = boot + b'\xff' * (131072 - len(boot)) + app
        (build / f'{args.hardware}-{role}-factory.bin').write_bytes(factory)
        stack = int.from_bytes(app[:4], 'little')
        reset = int.from_bytes(app[4:8], 'little')
        assert 0x20000000 < stack <= 0x20020000 and stack % 8 == 0
        assert reset & 1 and APP_BASE < reset < APP_BASE + len(app)
        report = {'target': f'{args.hardware.upper()} STM32H723VGT6', 'hardware_profile': args.hardware,
                  'cc_controller': 'FUSB303BTMX' if args.hardware in ('c85', 'c86') else 'TUSB320LAIRWBR',
                  'role': role, 'version': DISPLAY_VERSION, 'version_number': VERSION,
                  'app_address': '0x08020000', 'factory_address': '0x08000000',
                  'app_bytes': len(app), 'boot_bytes': len(boot), 'factory_bytes': len(factory),
                  'settings_sectors': [6, 7], 'hardware_validated': False, 'scan_target_hz': scan_hz,
                  'adc_resolution_bits': 14 if dual96_14bit else 12,
                  'adc_dual64': dual64,
                  'adc_dual96_12bit': args.adc_dual96_12bit,
                  'adc_dual64_matched40': args.adc_dual64_matched40,
                  'adc_capture_diag': args.adc_capture_diag,
                  'cpu_profile': args.adc_capture_diag and not args.adc_isr_probe,
                  'adc_irq_probe': args.adc_isr_probe,
                  'adc_async64': args.adc_async64,
                  'adc_async96': args.adc_async96,
                  'adc_instances': [1, 2] if scan_hz == 96000 or dual64 else [1],
                  'adc_ranks': [4, 3] if scan_hz == 96000 or dual64 else [7],
                  'adc_effective_clock_hz': 40000000 if args.adc_capture_diag or async_adc else (30000000 if dual12 else (20000000 if dual96_14bit else (40000000 if scan_hz in (64000, 75000) else 12500000))),
                  'adc_sample_cycles': 32.5 if dual96_14bit else 64.5,
                  'configuration_adc_units': 'legacy12-bit; no stored endpoint migration',
                  'hall_decision_precision': 'Q5 legacy ADC units;8-sample sum of14-bit samples' if dual96_14bit else 'legacy integer ADC',
                  'split_poll_target_hz': 8000, 'usb_hs_interval_us': 125 if role == 'left' else None,
                  'compiler': compiler_version,
                  'files': {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                            for path in build.glob('*') if path.suffix in ('.bin', '.hex', '.elf')
                            or path.name.endswith('.fw.json')}}
        if args.adc_dual64:
            report.update({'dma_frames_per_irq': 1, 'publication_target_hz': 64000,
                           'filter_samples': 8, 'filter_execution': 'per-scan IRQ running sum',
                           'adc_longest_conversion_us': 4 * (64.5 + 6.5) / 30,
                           'adc_acquisition_us_per_channel': 64.5 / 30})
        if args.adc_dual96_12bit:
            report.update({'dma_frames_per_irq': 2, 'publication_target_hz': 48000,
                           'filter_samples': 8, 'filter_execution': 'per-batch IRQ running sum over both scans',
                           'sample_frame_bytes': 48,
                           'adc_longest_conversion_us': 4 * (64.5 + 6.5) / 30,
                           'adc_conversion_with_trigger_latency_us': (4 * (64.5 + 6.5) + 3.5) / 30,
                           'adc_acquisition_us_per_channel': 64.5 / 30})
        if scan_hz == 75000:
            report.update({'dma_frames_per_irq': 1, 'publication_target_hz': 75000,
                           'filter_samples': 8, 'filter_execution': 'per-scan IRQ running sum',
                           'adc_longest_conversion_us': 7 * (64.5 + 6.5) / 40,
                           'adc_conversion_with_trigger_latency_us': (7 * (64.5 + 6.5) + 3.5) / 40,
                           'adc_acquisition_us_per_channel': 64.5 / 40})
        if args.adc_capture_diag:
            longest_ranks = 4 if args.adc_dual64_matched40 else 7
            report.update({'dma_frames_per_irq': 1, 'publication_target_hz': 64000,
                           'filter_samples': 8, 'filter_execution': 'per-scan IRQ running sum',
                           'sample_frame_bytes': 48,
                           'adc_longest_conversion_us': longest_ranks * (64.5 + 6.5) / 40,
                           'adc_conversion_with_trigger_latency_us': (longest_ranks * (64.5 + 6.5) + 3.5) / 40,
                           'adc_acquisition_us_per_channel': 64.5 / 40})
        if async_adc:
            paired = args.adc_async96
            longest_cycles = 8 * (32.5 + 6.5) if paired else 4 * (64.5 + 6.5)
            report.update({'adc_ranks': [8, 5] if paired else [4, 3],
                           'adc1_phase_paired': paired,
                           'adc2_phase_paired': paired,
                           'adc2_phase_pair_sample_cycles': 32.5 if paired else None,
                           'adc2_phase_pair_filter_scans': 1 if paired else None,
                           'dma_scans_per_bank': 32, 'dma_frames_per_irq': 16,
                           'dma_irq_target_hz_per_bank': scan_hz // 16, 'publication_target_hz': None,
                           'publication_mode': 'foreground latest completed per bank',
                           'filter_samples': 8, 'filter_execution': 'foreground per-ADC eight-sample average',
                           'sample_frame_bytes': 48,
                           'adc_longest_conversion_us': longest_cycles / 40,
                           'adc_conversion_with_trigger_latency_us': (longest_cycles + 3.5) / 40,
                           'adc_acquisition_us_per_channel': 64.5 / 40,
                           'adc_sequence_meaning': 'minimum completed scan cursor across both ADC banks'})
        if args.adc_async96 and role == 'right':
            report.update({'split_reply_bytes': 32,
                           'split_hall_input': '12-bit configured per-channel average'})
        report['hardware_evidence_sha256'] = evidence
        if profile:
            report['native_board_sha256'] = profile['native_board_sha256']
        if args.hardware == 'c86':
            report.update({'permanent_updater': True, 'update_protocol': 1,
                           'app_max_bytes': MAX_IMAGE, 'app_metadata_address': hex(METADATA_BASE),
                           'app_update_bundle': f'c86-{role}-app.fw.json',
                           'factory_install_requires_swd': True,
                           'power_loss_hardware_qualified': False})
        if input_evidence != source_evidence():
            raise RuntimeError('Build inputs changed while compiling; repeat build')
        report.update(input_evidence)
        validate_build_info(report, app, args.hardware, role)
        (build / 'build-info.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(f'{role}: app {len(app)} bytes; boot {len(boot)} bytes; {scan_hz}Hz target; hardware test pending')


if __name__ == '__main__':
    main()
