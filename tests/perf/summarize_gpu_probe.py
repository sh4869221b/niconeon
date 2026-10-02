#!/usr/bin/env python3
"""Validate/describe bounded overlay GL timestamp probes, never performance acceptance."""
import hashlib
import json
import math
from pathlib import Path
import sys


def summarize_checked(root):
    manifest = json.loads((root / 'manifest.json').read_text())
    errors, runs = [], []
    if not manifest.get('complete') or len(manifest.get('runs', [])) != 4:
        errors.append('Incomplete campaign')
    for run in manifest['runs']:
        path = (root / run['raw_path']).resolve()
        if not path.is_relative_to(root.resolve()):
            errors.append('Raw path escapes campaign')
            continue
        if hashlib.sha256(path.read_bytes()).hexdigest() != run['raw_sha256']:
            errors.append(run['run_id'] + ': raw hash mismatch')
        raw = json.loads(path.read_text())
        meta = raw['metadata']
        start, end = meta['measurement_start_elapsed_ns'], meta['measurement_end_elapsed_ns']
        frames = raw['render_samples']
        seen_sequences = set()
        intervals = []
        with_draw = []
        skipped = sum(x.get('gpu_skipped_queries', 0) for x in frames)
        invalid = sum(x.get('gpu_invalid_results', 0) for x in frames)
        pending = [x.get('gpu_pending_queries', -1) for x in frames]
        if (meta.get('gpu_timing_requested') is not True or not frames or invalid
                or any(x < 0 or x > 8 for x in pending)):
            errors.append(run['run_id'] + ': invalid/missing timestamp coverage or bounds')
        for frame in frames:
            if not frame.get('gpu_result_available'):
                continue
            at = frame['gpu_measured_cpu_start_elapsed_ns']
            ns = frame['gpu_elapsed_ns']
            sequence = frame['gpu_measured_frame_sequence']
            if (sequence in seen_sequences or sequence > frame['frame_sequence']
                    or not isinstance(ns, int) or not isinstance(at, int)
                    or not frame.get('gpu_timing_supported') or not frame.get('gpu_timing_requested')
                    or ns < 0 or at > frame['elapsed_ns']):
                errors.append(run['run_id'] + ': malformed completed query')
                continue
            seen_sequences.add(sequence)
            if start <= at <= end:
                intervals.append(ns)
                if frame['gpu_measured_draw_calls'] > 0:
                    with_draw.append(ns)
        if len(with_draw) < 100 or skipped:
            errors.append(run['run_id'] + ': insufficient or skipped query coverage')
        def distribution(values):
            values = sorted(values)
            return {'count': len(values), 'p50_ms': values[math.ceil(len(values)*.5)-1]/1e6,
                    'p95_ms': values[math.ceil(len(values)*.95)-1]/1e6,
                    'p99_ms': values[math.ceil(len(values)*.99)-1]/1e6,
                    'total_ms': sum(values)/1e6} if values else {'count': 0}
        runs.append({'run_id': run['run_id'], 'variant': run['variant'],
                     'raw_quality_success': raw['success'], 'raw_quality_errors': raw['errors'],
                     'summary': {k: v for k, v in raw['summary'].items() if not isinstance(v, (dict, list))},
                     'all_overlay_intervals': distribution(intervals),
                     'overlay_intervals_with_draw': distribution(with_draw),
                     'skipped_queries': skipped, 'invalid_results': invalid,
                     'max_pending_queries': max(pending, default=0),
                     'final_pending_queries': pending[-1] if pending else 0})
    return {'kind': 'niconeon.overlay-gl-probe', 'errors': errors, 'runs': runs,
            'scope': 'Diagnostic GL timestamp brackets, including uploads and command-producer gaps. Not frame-time acceptance, scanout, GPU utilization, or hardware qualification. Raw quality failures remain failed.'}


def summarize(root):
    try:
        return summarize_checked(root)
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        return {'kind': 'niconeon.overlay-gl-probe', 'errors': ['Malformed probe: ' + str(error)], 'runs': []}


if __name__ == '__main__':
    root = Path(sys.argv[1]).resolve()
    report = summarize(root)
    with (root / 'gpu-analysis.json').open('x') as out:
        json.dump(report, out, indent=2)
    print(json.dumps(report, indent=2))
    raise SystemExit(2 if report['errors'] else 0)
