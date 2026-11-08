"""Run meaningful offline tests; no board access. Requires a native C compiler."""
from pathlib import Path
import argparse,os,subprocess,sys
ROOT=Path(__file__).resolve().parents[1]
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--firmware',action='store_true');ap.add_argument('--toolchain');ap.add_argument('--studio', type=Path);a=ap.parse_args()
    cmds=[
      [sys.executable,'tests/run_core_tests.py'],
      [sys.executable,'tests/run_board_tests.py'],
      [sys.executable,'tests/run_adc_tests.py'],
      [sys.executable,'tests/run_cc_tests.py'],
      [sys.executable,'tests/run_usb_tests.py'],
      [sys.executable,'tests/run_update_tests.py'],
      [sys.executable,'tests/run_runtime_tests.py'],
      [sys.executable,'tests/run_split_link_tests.py'],
      [sys.executable,'-m','unittest','tests.host_test'],
      [sys.executable,'-m','unittest','tests.image_artifacts_test'],
      [sys.executable,'-m','unittest','tests.build_profile_cli_test'],
      [sys.executable,'-m','unittest','tests.host_update_test'],
      [sys.executable,'-m','unittest','tests.host_performance_test'],
      [sys.executable,'-m','unittest','tests.host_session_diagnostics_test','tests.host_mcp_configuration_test','tests.host_mcp_calibration_test','tests.host_mcp_maintenance_test'],
      [sys.executable,'-m','unittest','tests.host_key_timing_test'],
      [sys.executable,'-m','unittest','tests.host_hall_curve_test'],
      [sys.executable,'tests/host_mcp_e2e.py'],
      [sys.executable,'tests/host_core_bridge.py'],
      [sys.executable,'tests/hall_curve_parity.py'],
    ]
    if a.firmware:
        extra=['--toolchain',a.toolchain] if a.toolchain else []
        for hardware in ('c77','c85','c86'):
            cmds.extend([[sys.executable,'build.py','--role','both','--hardware',hardware,*extra],[sys.executable,'tools/verify_images.py','--hardware',hardware,*extra]])
        cmds.extend([[sys.executable,'build.py','--role','both','--hardware','c86','--scan-hz','16000',*extra],
                     [sys.executable,'tools/verify_images.py','--hardware','c86','--scan-hz','16000',*extra]])
        cmds.extend([[sys.executable,'build.py','--role','left','--hardware','c86','--scan-hz','75000',*extra],
                     [sys.executable,'tools/verify_images.py','--role','left','--hardware','c86','--scan-hz','75000',*extra]])
        cmds.extend([[sys.executable,'build.py','--role','left','--hardware','c86','--scan-hz','96000',*extra],
                     [sys.executable,'tools/verify_images.py','--role','left','--hardware','c86','--scan-hz','96000',*extra]])
        cmds.extend([[sys.executable,'build.py','--role','left','--hardware','c86','--adc-dual64',*extra],
                     [sys.executable,'tools/verify_images.py','--role','left','--hardware','c86','--adc-dual64',*extra]])
        cmds.extend([[sys.executable,'build.py','--role','left','--hardware','c86','--scan-hz','96000','--adc-dual96-12bit',*extra],
                     [sys.executable,'tools/verify_images.py','--role','left','--hardware','c86','--scan-hz','96000','--adc-dual96-12bit',*extra]])
        cmds.extend([[sys.executable,'build.py','--role','left','--hardware','c86','--scan-hz','64000','--adc-async64',*extra],
                     [sys.executable,'tools/verify_images.py','--role','left','--hardware','c86','--scan-hz','64000','--adc-async64',*extra]])
        cmds.extend([[sys.executable,'build.py','--role','both','--hardware','c86','--scan-hz','96000','--adc-async96',*extra],
                     [sys.executable,'tools/verify_images.py','--role','both','--hardware','c86','--scan-hz','96000','--adc-async96',*extra]])
        for profile in ([],['--adc-dual64-matched40']):
            cmds.extend([[sys.executable,'build.py','--role','left','--hardware','c86','--scan-hz','64000','--adc-capture-diag',*profile,*extra],
                         [sys.executable,'tools/verify_images.py','--role','left','--hardware','c86','--scan-hz','64000','--adc-capture-diag',*profile,*extra]])
            cmds.extend([[sys.executable,'build.py','--role','left','--hardware','c86','--scan-hz','64000','--adc-capture-diag','--adc-isr-probe',*profile,*extra],
                         [sys.executable,'tools/verify_images.py','--role','left','--hardware','c86','--scan-hz','64000','--adc-capture-diag','--adc-isr-probe',*profile,*extra]])
    if a.studio:
        studio = a.studio.resolve()
        if not (studio / 'web/protocol.mjs').is_file():
            ap.error('--studio must point to the Split-Beat HE Studio checkout')
        os.environ['SPLIT_BEAT_STUDIO'] = str(studio)
        cmds.append(['node', 'tests/host_core_bridge.mjs'])
    else:
        os.environ.pop('SPLIT_BEAT_STUDIO', None)
    for cmd in cmds:
        print('RUN '+' '.join(cmd),flush=True);subprocess.run(cmd,cwd=ROOT,check=True)
    print('All offline checks PASS. No hardware qualification performed.',flush=True)
if __name__=='__main__':main()
