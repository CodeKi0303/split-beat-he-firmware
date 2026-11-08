"""Wrong target/profile combinations must fail before building or packaging."""
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BuildProfileCLI(unittest.TestCase):
    def test_dual64_rejects_other_targets_and_rates(self):
        for tool in ('build.py', 'tools/verify_images.py', 'tools/package_release.py'):
            for args in (
                ['--hardware', 'c77', '--role', 'left'],
                ['--hardware', 'c85', '--role', 'left'],
                ['--hardware', 'c86', '--role', 'right'],
                ['--hardware', 'c86', '--role', 'both'],
                ['--hardware', 'c86', '--role', 'left', '--scan-hz', '16000'],
                ['--hardware', 'c86', '--role', 'left', '--scan-hz', '75000'],
                ['--hardware', 'c86', '--role', 'left', '--scan-hz', '96000'],
            ):
                with self.subTest(tool=tool, args=args):
                    result = subprocess.run([sys.executable, tool, '--adc-dual64', *args],
                                            cwd=ROOT, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn('--adc-dual64 requires', result.stderr)

    def test_single75_rejects_other_hardware_and_roles(self):
        for tool in ('build.py', 'tools/verify_images.py', 'tools/package_release.py'):
            for args, error in (
                (['--hardware', 'c77', '--role', 'left'], 'High-rate scanning'),
                (['--hardware', 'c85', '--role', 'left'], 'High-rate scanning'),
                (['--hardware', 'c86', '--role', 'right'], '75kHz is a left-only'),
                (['--hardware', 'c86', '--role', 'both'], '75kHz is a left-only'),
                (['--hardware', 'c86'], '75kHz is a left-only'),
            ):
                with self.subTest(tool=tool, args=args):
                    result = subprocess.run([sys.executable, tool, '--scan-hz', '75000', *args],
                                            cwd=ROOT, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn(error, result.stderr)


    def test_dual96_12bit_requires_explicit_target_rate_and_no_dual64(self):
        for tool in ('build.py', 'tools/verify_images.py', 'tools/package_release.py'):
            for args, error in (
                (['--hardware', 'c77', '--role', 'left'], '--adc-dual96-12bit requires'),
                (['--hardware', 'c85', '--role', 'left'], '--adc-dual96-12bit requires'),
                (['--hardware', 'c77', '--role', 'left', '--scan-hz', '96000'], 'High-rate scanning'),
                (['--hardware', 'c85', '--role', 'left', '--scan-hz', '96000'], 'High-rate scanning'),
                (['--hardware', 'c86', '--role', 'right', '--scan-hz', '96000'], '96kHz is a left-only'),
                (['--hardware', 'c86', '--role', 'both', '--scan-hz', '96000'], '96kHz is a left-only'),
                (['--hardware', 'c86', '--scan-hz', '96000'], '96kHz is a left-only'),
                (['--hardware', 'c86', '--role', 'left'], '--adc-dual96-12bit requires'),
                (['--hardware', 'c86', '--role', 'left', '--scan-hz', '16000'], '--adc-dual96-12bit requires'),
                (['--hardware', 'c86', '--role', 'left', '--scan-hz', '64000'], '--adc-dual96-12bit requires'),
                (['--hardware', 'c86', '--role', 'left', '--scan-hz', '75000'], '--adc-dual96-12bit requires'),
                (['--hardware', 'c86', '--role', 'left', '--scan-hz', '96000', '--adc-dual64'], '--adc-dual96-12bit requires'),
            ):
                with self.subTest(tool=tool, args=args):
                    result = subprocess.run([sys.executable, tool, '--adc-dual96-12bit', *args],
                                            cwd=ROOT, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn(error, result.stderr)


    def test_capture_diag_rejects_unmatched_profiles(self):
        for tool in ('build.py', 'tools/verify_images.py', 'tools/package_release.py'):
            for args in (
                ['--hardware', 'c77', '--role', 'left'],
                ['--hardware', 'c85', '--role', 'left'],
                ['--hardware', 'c86', '--role', 'right'],
                ['--hardware', 'c86', '--role', 'both'],
                ['--hardware', 'c86', '--role', 'left', '--scan-hz', '16000'],
                ['--hardware', 'c86', '--role', 'left', '--scan-hz', '75000'],
                ['--hardware', 'c86', '--role', 'left', '--scan-hz', '96000'],
                ['--hardware', 'c86', '--role', 'left', '--adc-dual64'],
                ['--hardware', 'c86', '--role', 'left', '--scan-hz', '96000', '--adc-dual96-12bit'],
                ['--hardware', 'c86', '--role', 'right', '--adc-dual64-matched40'],
                ['--hardware', 'c86', '--role', 'left', '--adc-dual64', '--adc-dual64-matched40'],
            ):
                with self.subTest(tool=tool, args=args):
                    result = subprocess.run([sys.executable, tool, '--adc-capture-diag', *args],
                                            cwd=ROOT, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn('--adc-capture-diag requires', result.stderr)

    def test_matched40_requires_capture_diag(self):
        for tool in ('build.py', 'tools/verify_images.py', 'tools/package_release.py'):
            for extra in ([], ['--adc-dual64'], ['--scan-hz', '96000', '--adc-dual96-12bit']):
                with self.subTest(tool=tool, extra=extra):
                    result = subprocess.run([sys.executable, tool, '--hardware', 'c86', '--role', 'left',
                                             '--adc-dual64-matched40', *extra],
                                            cwd=ROOT, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn('--adc-dual64-matched40 requires --adc-capture-diag', result.stderr)

    def test_isr_probe_requires_capture_diag(self):
        for tool in ('build.py', 'tools/verify_images.py', 'tools/package_release.py'):
            for extra in ([], ['--adc-dual64-matched40'], ['--adc-dual64'],
                          ['--scan-hz', '96000', '--adc-dual96-12bit']):
                with self.subTest(tool=tool, extra=extra):
                    result = subprocess.run([sys.executable, tool, '--hardware', 'c86', '--role', 'left',
                                             '--adc-isr-probe', *extra],
                                            cwd=ROOT, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn('--adc-isr-probe requires --adc-capture-diag', result.stderr)

    def test_isr_probe_rejects_unmatched_profiles(self):
        for tool in ('build.py', 'tools/verify_images.py', 'tools/package_release.py'):
            for args in (
                ['--hardware', 'c77', '--role', 'left'],
                ['--hardware', 'c85', '--role', 'left'],
                ['--hardware', 'c86', '--role', 'right'],
                ['--hardware', 'c86', '--role', 'both'],
                ['--hardware', 'c86', '--role', 'left', '--scan-hz', '16000'],
                ['--hardware', 'c86', '--role', 'left', '--scan-hz', '75000'],
                ['--hardware', 'c86', '--role', 'left', '--scan-hz', '96000'],
                ['--hardware', 'c86', '--role', 'left', '--adc-dual64'],
                ['--hardware', 'c86', '--role', 'left', '--scan-hz', '96000', '--adc-dual96-12bit'],
                ['--hardware', 'c86', '--role', 'right', '--adc-dual64-matched40'],
                ['--hardware', 'c86', '--role', 'left', '--adc-dual64', '--adc-dual64-matched40'],
            ):
                with self.subTest(tool=tool, args=args):
                    result = subprocess.run([sys.executable, tool, '--adc-capture-diag', '--adc-isr-probe', *args],
                                            cwd=ROOT, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn('--adc-capture-diag requires', result.stderr)

    def test_async64_requires_explicit_matched_target_and_no_other_modes(self):
        for tool in ('build.py', 'tools/verify_images.py', 'tools/package_release.py'):
            cases = [
                (['--hardware', 'c77', '--role', 'left'], '--adc-async64 requires'),
                (['--hardware', 'c85', '--role', 'left'], '--adc-async64 requires'),
                (['--hardware', 'c77', '--role', 'left', '--scan-hz', '64000'], 'High-rate scanning'),
                (['--hardware', 'c85', '--role', 'left', '--scan-hz', '64000'], 'High-rate scanning'),
                (['--hardware', 'c86', '--role', 'right', '--scan-hz', '64000'], '--adc-async64 requires'),
                (['--hardware', 'c86', '--role', 'both', '--scan-hz', '64000'], '--adc-async64 requires'),
                (['--hardware', 'c86', '--scan-hz', '64000'], '--adc-async64 requires'),
                (['--hardware', 'c86', '--role', 'left'], '--adc-async64 requires'),
            ]
            cases += [(['--hardware', 'c86', '--role', 'left', '--scan-hz', str(rate)],
                       '--adc-async64 requires') for rate in (16000, 75000, 96000)]
            cases += [(['--hardware', 'c86', '--role', 'left', '--scan-hz', '64000', mode],
                       '--adc-async64 requires') for mode in ('--adc-dual64', '--adc-dual96-12bit',
                       '--adc-capture-diag', '--adc-dual64-matched40', '--adc-isr-probe', '--adc-async96')]
            for args, error in cases:
                with self.subTest(tool=tool, args=args):
                    result = subprocess.run([sys.executable, tool, '--adc-async64', *args],
                                            cwd=ROOT, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn(error, result.stderr)

    def test_async96_requires_explicit_matched_target_and_no_other_modes(self):
        for tool in ('build.py', 'tools/verify_images.py', 'tools/package_release.py'):
            cases = [
                (['--hardware', 'c77', '--role', 'left'], '--adc-async96 requires'),
                (['--hardware', 'c85', '--role', 'left'], '--adc-async96 requires'),
                (['--hardware', 'c77', '--role', 'left', '--scan-hz', '96000'], 'High-rate scanning'),
                (['--hardware', 'c85', '--role', 'left', '--scan-hz', '96000'], 'High-rate scanning'),
                (['--hardware', 'c86', '--role', 'left'], '--adc-async96 requires'),
                (['--hardware', 'c86', '--role', 'right'], '--adc-async96 requires'),
                (['--hardware', 'c86', '--role', 'both', '--scan-hz', '64000'], '--adc-async96 requires'),
                (['--hardware', 'c86', '--role', 'left', '--scan-hz', '96000', '--adc-async64'],
                 '--adc-async64 requires'),
            ]
            cases += [(['--hardware', 'c86', '--role', 'left', '--scan-hz', str(rate)],
                       '--adc-async96 requires') for rate in (16000, 64000, 75000)]
            cases += [(['--hardware', 'c86', '--role', 'left', '--scan-hz', '96000', mode],
                       '--adc-async96 requires') for mode in ('--adc-dual64', '--adc-dual96-12bit',
                       '--adc-capture-diag', '--adc-dual64-matched40', '--adc-isr-probe')]
            for args, error in cases:
                with self.subTest(tool=tool, args=args):
                    result = subprocess.run([sys.executable, tool, '--adc-async96', *args],
                                            cwd=ROOT, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn(error, result.stderr)

    def test_right_96k_requires_async_profile(self):
        for tool in ('build.py', 'tools/verify_images.py', 'tools/package_release.py'):
            for role in ('right', 'both'):
                with self.subTest(tool=tool, role=role):
                    result = subprocess.run([sys.executable, tool, '--hardware', 'c86',
                                             '--role', role, '--scan-hz', '96000'],
                                            cwd=ROOT, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn('96kHz is a left-only experiment without --adc-async96', result.stderr)


if __name__ == '__main__':
    unittest.main()
