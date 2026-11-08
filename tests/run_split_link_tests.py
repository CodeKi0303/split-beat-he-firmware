"""Compile production split-link interrupt/handshake paths with host registers."""
import subprocess
from run_core_tests import ROOT, compile_target


def main():
    crc_exe=compile_target('test_split_crc',[ROOT/'tests/test_split_crc.c'],[])
    subprocess.run([str(crc_exe)],check=True)
    source=(ROOT/'src/split_link.c').read_text(encoding='utf-8')
    # Exclude MCU-only initializer, retaining the exact RX/TX/IRQ/transition code.
    first=source[source.index('#define RX_SIZE'):source.index('\nvoid split_link_start(')]
    second=source[source.index('\nvoid split_link_allow('):]
    build=ROOT/'build/host';build.mkdir(parents=True,exist_ok=True)
    (build/'split_link_under_test.inc').write_text(first+second,encoding='utf-8')
    sources=[ROOT/'tests/test_split_link.c',ROOT/'src/split_codec.c',ROOT/'src/update_link_codec.c']
    for left in (0,1):
        exe=compile_target(f'test_split_link_{left}',sources,[f'ROLE_LEFT={left}','HARDWARE_REVISION=86'])
        subprocess.run([str(exe)],check=True)


if __name__=='__main__':
    main()
    import run_split_link_guard_tests
