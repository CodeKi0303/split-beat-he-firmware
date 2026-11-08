"""Compile the real updater/backend against fault-injected flash; no hardware."""
import subprocess
from run_core_tests import ROOT, compile_target

def main():
    sources = [ROOT / p for p in (
        'src/fw_update.c', 'src/fw_update_flash.c', 'tests/test_fw_update.c')]
    for revision, role_left, boot in ((86, 1, 1), (86, 0, 1), (86, 1, 0), (86, 0, 0), (77, 1, 1), (77, 1, 0)):
        name = f'test_fw_update_c{revision}_{role_left}_{boot}'
        exe = compile_target(name, sources, [
            'FW_UPDATE_HOST_TEST=1', f'HARDWARE_REVISION={revision}',
            f'ROLE_LEFT={role_left}', f'FW_UPDATE_BOOT={boot}'])
        subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    main()
