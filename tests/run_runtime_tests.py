"""Exercise actual updater orchestration/codec with mocks, without hardware."""
import subprocess
from run_core_tests import ROOT, compile_target

def main():
    sources = [ROOT / p for p in (
        'src/fw_runtime.c', 'src/update_link_codec.c', 'tests/test_fw_runtime.c')]
    for role_left, boot in ((1, 0), (1, 1), (0, 0), (0, 1)):
        exe = compile_target(f'test_fw_runtime_{role_left}_{boot}', sources, [
            'HARDWARE_REVISION=86', f'ROLE_LEFT={role_left}', f'FW_UPDATE_BOOT={boot}'])
        subprocess.run([str(exe)], check=True)
    diagnostic_sources = [ROOT / p for p in (
        'src/performance.c', 'src/update_link_codec.c', 'tests/test_performance.c')]
    diagnostic_exe = compile_target('test_performance', diagnostic_sources, [
        'HARDWARE_REVISION=86', 'ROLE_LEFT=1'])
    subprocess.run([str(diagnostic_exe)], check=True)
    trace_sources = [ROOT / p for p in ('src/update_link_codec.c', 'tests/test_adc_trace.c')]
    trace_exe = compile_target('test_adc_trace', trace_sources, [
        'HARDWARE_REVISION=86', 'ROLE_LEFT=1', 'C86_ADC_CAPTURE_DIAG=1'])
    subprocess.run([str(trace_exe)], check=True)
    cpu_sources = [ROOT / p for p in ('src/cpu_profile.c', 'src/update_link_codec.c', 'tests/test_cpu_profile.c')]
    cpu_exe = compile_target('test_cpu_profile', cpu_sources, [
        'HARDWARE_REVISION=86', 'ROLE_LEFT=1', 'C86_ADC_CAPTURE_DIAG=1',
        'C86_CPU_PROFILE=1', 'CPU_PROFILE_HOST_TEST=1'])
    subprocess.run([str(cpu_exe)], check=True)
    probe_sources = [ROOT / p for p in ('src/adc_irq_probe.c', 'src/update_link_codec.c', 'tests/test_adc_irq_probe.c')]
    probe_exe = compile_target('test_adc_irq_probe', probe_sources, [
        'HARDWARE_REVISION=86', 'ROLE_LEFT=1', 'C86_ADC_CAPTURE_DIAG=1',
        'C86_ADC_IRQ_PROBE=1', 'ADC_IRQ_PROBE_HOST_TEST=1'])
    subprocess.run([str(probe_exe)], check=True)

if __name__ == '__main__':
    main()
