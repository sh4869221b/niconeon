#!/usr/bin/env python3
"""Stdlib-only evidence-tool tests; the fake harness is NOT an OpenGL test.

Run: python3 -B tests/perf/test_profile_analysis.py
No compiler, Qt runtime, display, real video, or third-party package is needed.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


analysis = load_module("analyze_real_render", HERE / "analyze_real_render.py")
runner = load_module("run_real_render_comparison", HERE / "run_real_render_comparison.py")

# A process-level fixture tests the CLI/manifest contract without exercising GL.
FAKE_HARNESS = '''#!/usr/bin/env python3
import argparse, hashlib, json, pathlib
parser = argparse.ArgumentParser()
for name in ('video', 'output', 'cps', 'duration-ms', 'tail-ms', 'text-mode',
             'sample-mode', 'worker', 'renderer'):
    parser.add_argument('--' + name, required=True)
args = parser.parse_args()
expected = int(args.cps) * int(args.duration_ms) // 1000
candidate = 'candidate' in pathlib.Path(__file__).name
interval = 15000000 if candidate else 16666667
samples = [{'elapsed_ns': 1000000000 + (i + 1) * interval, 'interval_ns': interval, 'phase': 'feed'}
           for i in range(1001)]
samples += [{'elapsed_ns': 31000000000, 'interval_ns': 100000000, 'phase': 'transition'}]
samples += [{'elapsed_ns': 31000000000 + (i + 1) * interval, 'interval_ns': interval, 'phase': 'drain'}
            for i in range(500)]
metadata = {
    'cps': int(args.cps), 'duration_ms': int(args.duration_ms),
    'tail_ms': int(args.tail_ms), 'text_mode': args.text_mode,
    'sample_mode': args.sample_mode, 'worker': args.worker, 'renderer': args.renderer,
    'video_started': True, 'media_start_ms': 0, 'media_end_ms': 45000, 'qt_version': 'fake-qt',
    'measurement_start_elapsed_ns': 1000000000, 'feed_end_elapsed_ns': 31000000000,
    'measurement_end_elapsed_ns': 46000000000,
    'mpv_version': 'fake-mpv', 'gl_renderer': 'fake-not-a-real-GL-test',
    'video_sha256': hashlib.sha256(pathlib.Path(args.video).read_bytes()).hexdigest(),
    'snapshot_tracking': 'not sampled; actual draw IDs are the oracle',
}
summary = dict(expected=expected, offered=expected, accepted=expected,
               submitted_unique=expected, snapshot_unique=-1,
               raster_counters_available=False, never_submitted_ids=[])
for key in ('pending', 'failed', 'expired', 'missing_sprites', 'diagnostics_overflow',
            'unexpected_ids', 'source_pending', 'dropped', 'sample_overflow',
            'missing_image_observations', 'unresident_observations'):
    summary[key] = 0
lateness = 50000000 if candidate else 100000000
submissions = [{'comment_id': f'perf-{i}', 'elapsed_ns': 1000000000 + i * 1000000000 // int(args.cps) + lateness}
               for i in range(expected)]
summary.update(feed_offered=expected, feed_accepted=expected,
               feed_submitted_unique=sum(value['elapsed_ns'] <= 31000000000 for value in submissions),
               last_submission_elapsed_ns=submissions[-1]['elapsed_ns'])
raw = dict(format_version=1, success=True, errors=[], metadata=metadata,
           summary=summary, frame_samples=samples, heartbeat_samples=samples, submission_samples=submissions)
if args.sample_mode == 'pixels':
    raw.update(summary={}, frame_samples=[], heartbeat_samples=[],
               pixel_checks=[{'success': True, 'text': 'fake pixel oracle'}])
pathlib.Path(args.output).write_text(json.dumps(raw), encoding='utf-8')
'''


class StatisticsTests(unittest.TestCase):
    def test_nearest_rank(self):
        self.assertEqual(analysis.nearest_rank(list(range(1, 101)), 0.95), 95)
        self.assertEqual(analysis.nearest_rank(list(range(1, 101)), 0.99), 99)
        self.assertEqual(analysis.nearest_rank([8], 0.95), 8)
        self.assertEqual(analysis.nearest_rank([9, 2, 6], 0.50), 6)
        with self.assertRaises(ValueError):
            analysis.nearest_rank([], 0.95)

    def test_bootstrap_pairs_and_reproducibility(self):
        result = analysis.paired_bootstrap([1, 10], [2, 20], 1000, 42)
        self.assertAlmostEqual(result['geometric_mean_ratio'], 2)
        self.assertEqual(result['ci95_ratio'], [2, 2])
        self.assertEqual(result, analysis.paired_bootstrap([1, 10], [2, 20], 1000, 42))
        with self.assertRaises(ValueError):
            analysis.paired_bootstrap([1], [1], 1000, 42)

    def test_deterministic_balanced_order(self):
        args = argparse.Namespace(seed=42, pairs=10, cps=[100, 200, 400],
                                  text_modes=['unique', 'warm'])
        runs = runner.plan_runs(args)
        self.assertEqual(runs, runner.plan_runs(args))
        self.assertEqual(len(runs), 120)
        for cps in args.cps:
            for mode in args.text_modes:
                case = [run for run in runs if run['cps'] == cps and run['text_mode'] == mode]
                baseline = [run for run in case if run['variant'] == 'baseline']
                self.assertEqual(sum(run['pair_order'] == 'AB' for run in baseline), 5)
                self.assertEqual(sum(run['pair_order'] == 'BA' for run in baseline), 5)
                for pair in range(10):
                    matching = [run for run in case if run['pair_index'] == pair]
                    self.assertEqual(len(matching), 2)
                    self.assertEqual(abs(matching[0]['ordinal'] - matching[1]['ordinal']), 1)
        args.seed = 43
        self.assertNotEqual(runs, runner.plan_runs(args))


@unittest.skipIf(os.name == 'nt', 'process fixture uses a POSIX executable shebang')
class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='niconeon-profile-tools-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.video = self.root / 'fixture.mp4'
        self.video.write_bytes(b'Synthetic script-test fixture, not a playable video')
        for name in ('baseline', 'candidate'):
            path = self.root / name
            # Use the interpreter running these tests, including venvs.
            path.write_text(FAKE_HARNESS.replace('#!/usr/bin/env python3', '#!' + sys.executable),
                            encoding='utf-8')
            path.chmod(0o700)
        self.directory = self.root / 'timing'
        self.run_campaign(self.directory)
        self.manifest_path = self.directory / 'manifest.json'
        self.manifest = json.loads(self.manifest_path.read_text(encoding='utf-8'))
        self.original_manifest = copy.deepcopy(self.manifest)
        self.raw_path = self.directory / self.manifest['runs'][0]['raw_path']
        self.original_raw = json.loads(self.raw_path.read_text(encoding='utf-8'))
        self.options = argparse.Namespace(noninferiority_percent=0, min_pairs=2,
                                          min_samples=1000, bootstrap_iterations=1000, seed=10)

    def run_campaign(self, output: Path, sample_mode: str = 'timing'):
        command = [sys.executable, '-B', str(HERE / 'run_real_render_comparison.py'),
                   '--baseline', str(self.root / 'baseline'), '--candidate', str(self.root / 'candidate'),
                   '--video', str(self.video), '--output-dir', str(output), '--pairs', '2',
                   '--cps', '100', '--text-modes', 'unique', '--cooldown-seconds', '0',
                   '--sample-mode', sample_mode]
        result = subprocess.run(command, text=True, capture_output=True, timeout=20, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def analyze(self):
        return analysis.analyze(self.manifest_path, self.options)

    def write_manifest(self):
        self.manifest_path.write_text(json.dumps(self.manifest), encoding='utf-8')

    def alter_raw(self, change):
        raw = copy.deepcopy(self.original_raw)
        change(raw)
        self.raw_path.write_text(json.dumps(raw), encoding='utf-8')
        self.manifest['runs'][0]['raw_sha256'] = hashlib.sha256(self.raw_path.read_bytes()).hexdigest()
        self.write_manifest()

    def assert_invalid(self):
        report = self.analyze()
        self.assertEqual(report['status'], 'invalid', report)
        self.assertTrue(report['errors'])
        self.assertTrue(all(not case['metrics'] for case in report['cases']))

    def test_complete_comparison_and_isolation(self):
        report = self.analyze()
        self.assertEqual(report['status'], 'noninferior', report)
        self.assertEqual(len(report['cases'][0]['metrics']), 8)
        self.assertEqual(report['cases'][0]['runs'][0]['frame_drain_samples'], 500)
        self.assertEqual(report['cases'][0]['runs'][0]['frame_transition_samples'], 1)
        self.assertEqual(report['cases'][0]['runs'][0]['frame_full_samples'], 1502)
        self.assertEqual(len(report['cases'][0]['lateness_metrics']), 2)
        self.assertTrue(all(value['status'] == 'no_increase' for value in report['cases'][0]['lateness_metrics'].values()))
        self.assertIn('not full issue acceptance', report['claim_scope'])
        self.assertTrue(any('lack raster counters' in value for value in report['limitations']))
        homes = [run['xdg_environment']['XDG_RUNTIME_DIR'] for run in self.manifest['runs']]
        self.assertEqual(len(set(homes)), 4)
        self.assertTrue(all(run['environment']['QT_HASH_SEED'] == '0' for run in self.manifest['runs']))
        self.assertEqual(self.manifest['video']['sha256'], hashlib.sha256(self.video.read_bytes()).hexdigest())

    def test_equal_work_and_failure_counters(self):
        for key in analysis.COUNT_EQUALS_EXPECTED:
            with self.subTest(count=key):
                self.alter_raw(lambda raw: raw['summary'].__setitem__(key, 2999))
                self.assert_invalid()
        for key in analysis.ZERO_COUNTS:
            with self.subTest(counter=key):
                self.alter_raw(lambda raw: raw['summary'].__setitem__(key, 1))
                self.assert_invalid()

    def test_low_samples_and_replicates_are_inconclusive(self):
        self.options.min_pairs = 10
        report = self.analyze()
        self.assertEqual(report['status'], 'inconclusive')
        self.assertFalse(report['cases'][0]['metrics'])
        self.options.min_pairs = 2
        for key in ('frame_samples', 'heartbeat_samples'):
            self.alter_raw(lambda raw: raw.__setitem__(key, raw[key][:999] + raw[key][1001:]))
            report = self.analyze()
            self.assertEqual(report['status'], 'inconclusive')
            self.assertFalse(report['cases'][0]['metrics'])

    def test_raw_hash_and_malformed_samples(self):
        self.raw_path.write_text(self.raw_path.read_text(encoding='utf-8') + ' ', encoding='utf-8')
        self.assert_invalid()
        for change in (
            lambda raw: raw['metadata'].__setitem__('video_started', False),
            lambda raw: raw['metadata'].__setitem__('media_start_ms', 10),
            lambda raw: raw['metadata'].__setitem__('media_end_ms', 44999),
            lambda raw: raw['summary'].__setitem__('never_submitted_ids', ['perf-1']),
            lambda raw: raw['submission_samples'][0].__setitem__('comment_id', 'perf-999999'),
            lambda raw: raw['submission_samples'][0].__setitem__('elapsed_ns', 999999999999),
            lambda raw: raw['summary'].__setitem__('feed_submitted_unique', 0),
            lambda raw: raw['metadata'].__setitem__('video_sha256', '0' * 64),
            lambda raw: raw['frame_samples'][0].__setitem__('interval_ns', 0),
            lambda raw: raw['frame_samples'][0].__setitem__('phase', 'unknown'),
            lambda raw: raw.__setitem__('summary', []),
        ):
            self.alter_raw(change)
            self.assert_invalid()

    def test_malformed_and_incomplete_manifest(self):
        for change in (
            lambda value: value.__setitem__('complete', False),
            lambda value: value['runs'][0].pop('ordinal'),
            lambda value: value['runs'].append(copy.deepcopy(value['runs'][0])),
            lambda value: value['variants']['candidate'].__setitem__('renderer', 'frame_image'),
            lambda value: value['runs'][1].__setitem__('xdg_environment', value['runs'][0]['xdg_environment']),
            lambda value: value['runs'][0]['environment'].__setitem__('QT_HASH_SEED', 'random'),
        ):
            self.manifest = copy.deepcopy(self.original_manifest)
            change(self.manifest)
            self.write_manifest()
            self.assert_invalid()

    def test_pixels_remain_correctness_only_and_cli_exit_code(self):
        directory = self.root / 'pixels'
        self.run_campaign(directory, 'pixels')
        report = analysis.analyze(directory / 'manifest.json', self.options)
        self.assertEqual(report['status'], 'inconclusive', report)
        self.assertEqual(report['cases'][0]['status'], 'correctness_only')
        output = self.root / 'pixel-analysis.json'
        result = subprocess.run([sys.executable, '-B', str(HERE / 'analyze_real_render.py'),
                                 str(directory), '--output', str(output)],
                                text=True, capture_output=True, timeout=20, check=False)
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertEqual(json.loads(output.read_text(encoding='utf-8'))['status'], 'inconclusive')
        # Existing raw evidence or reports cannot be overwritten by --output.
        result = subprocess.run([sys.executable, '-B', str(HERE / 'analyze_real_render.py'),
                                 str(directory), '--output', str(output)],
                                text=True, capture_output=True, timeout=20, check=False)
        self.assertEqual(result.returncode, 2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
