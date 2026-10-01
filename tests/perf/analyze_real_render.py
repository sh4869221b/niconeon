#!/usr/bin/env python3
"""Fail-closed analysis of paired real_render_profile evidence (Python stdlib).

  python3 tests/perf/analyze_real_render.py /tmp/render-comparison \
      --output /tmp/render-comparison/analysis.json

Exit codes: 0 = noninferiority established for every measured case/metric;
1 = regression established; 2 = invalid/incomplete/equal-work failure;
3 = inconclusive, insufficient replication/samples, or pixel-only evidence.

Each run's p95/p99 is the nearest-rank percentile of ALL feed-phase and full-window intervals,
not percentiles/medians of two-second summaries. Candidate/baseline log ratios
are paired by case and replicate; the percentile bootstrap resamples complete
pairs and estimates the geometric-mean ratio. CIs are pointwise 95%, not a
simultaneous family-wise confidence band. Passing every metric is required.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import statistics
import sys
from typing import Any

COUNT_EQUALS_EXPECTED = ("offered", "accepted", "submitted_unique")
ZERO_COUNTS = ("pending", "failed", "expired", "missing_sprites", "diagnostics_overflow",
               "unexpected_ids", "source_pending", "dropped", "sample_overflow",
               "missing_image_observations", "unresident_observations")
METRICS = ("frame_p95_ns", "frame_p99_ns", "heartbeat_p95_ns", "heartbeat_p99_ns",
           "frame_full_p95_ns", "frame_full_p99_ns", "heartbeat_full_p95_ns", "heartbeat_full_p99_ns")
LATENESS_METRICS = ("first_draw_lateness_p95_ns", "first_draw_lateness_p99_ns")
# Compare these when supplied; versions/drivers must not change between arms.
ENVIRONMENT_METADATA = (
    "qt_version", "mpv_version", "gl_vendor", "gl_renderer", "gl_version",
    "qpa_platform", "graphics_api", "viewport_width", "viewport_height", "dpr",
    "device_pixel_ratio", "font_family", "font_pixel_size", "simd_mode", "mpv_scale",
    "qt", "mpv_client_api", "font", "width", "height", "video_sha256", "media_start_ms",
)
LIMITATIONS = [
    "submitted_unique proves actual GL draw submission, not physical presentation or readable pixels",
    "frame_samples are frameSwapped intervals, not GPU completion timestamps",
    "timing mode is separate from pixel validation; pixel instrumentation cannot qualify timing",
    "this harness bypasses normal application QoS and does not qualify normal-QoS behavior",
    "results describe the recorded executable, video, driver, display, and machine only",
    "bootstrap CIs are pointwise 95%; shared host state and order effects may remain",
    "first-draw lateness is wall elapsed time minus the scheduled media target; media-clock rate/stalls affect it",
]


def integer(value: Any, *, positive: bool = False) -> bool:
    return type(value) is int and (value > 0 if positive else value >= 0)


def number(value: Any, *, positive: bool = False) -> bool:
    return (type(value) in (int, float) and math.isfinite(value)
            and (value > 0 if positive else value >= 0))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def valid_sha256(value: Any) -> bool:
    return (isinstance(value, str) and len(value) == 64
            and all(character in "0123456789abcdef" for character in value))


def nearest_rank(values: list[float | int], probability: float) -> float | int:
    """The conventional inverse empirical CDF: sorted[ceil(p*n)-1]."""
    if not values or not 0 < probability <= 1:
        raise ValueError("nearest_rank needs nonempty values and 0 < probability <= 1")
    return sorted(values)[max(0, math.ceil(probability * len(values)) - 1)]


def paired_bootstrap(baseline: list[float], candidate: list[float],
                     iterations: int, seed: int) -> dict[str, Any]:
    if len(baseline) != len(candidate) or len(baseline) < 2:
        raise ValueError("at least two complete pairs are required")
    log_ratios = [math.log(c / b) for b, c in zip(baseline, candidate)]
    rng = random.Random(seed)
    count = len(log_ratios)
    means = [statistics.fmean(log_ratios[rng.randrange(count)] for _ in range(count))
             for _ in range(iterations)]
    ratio = math.exp(statistics.fmean(log_ratios))
    lower = math.exp(nearest_rank(means, 0.025))
    upper = math.exp(nearest_rank(means, 0.975))
    return {
        "pair_count": count, "geometric_mean_ratio": ratio,
        "change_percent": (ratio - 1) * 100,
        "ci95_ratio": [lower, upper],
        "ci95_change_percent": [(lower - 1) * 100, (upper - 1) * 100],
        "pair_ratios": [math.exp(value) for value in log_ratios],
        "bootstrap_iterations": iterations, "bootstrap_seed": seed,
    }


def paired_difference_bootstrap(baseline: list[float], candidate: list[float],
                                iterations: int, seed: int) -> dict[str, Any]:
    """Descriptive lateness differences; not an additional frame acceptance gate."""
    if len(baseline) != len(candidate) or len(baseline) < 2:
        raise ValueError("at least two complete pairs are required")
    differences = [c - b for b, c in zip(baseline, candidate)]
    rng = random.Random(seed)
    count = len(differences)
    means = [statistics.fmean(differences[rng.randrange(count)] for _ in range(count))
             for _ in range(iterations)]
    lower, upper = nearest_rank(means, 0.025), nearest_rank(means, 0.975)
    return {
        "pair_count": count, "mean_paired_difference_ns": statistics.fmean(differences),
        "ci95_difference_ns": [lower, upper], "pair_differences_ns": differences,
        "status": "increased" if lower > 0 else "no_increase" if upper <= 0 else "inconclusive",
        "bootstrap_iterations": iterations, "bootstrap_seed": seed,
        "acceptance_gate": False,
    }


def sample_metrics(raw: dict[str, Any], context: str, errors: list[str]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for prefix, key in (("frame", "frame_samples"), ("heartbeat", "heartbeat_samples")):
        samples = raw.get(key)
        if not isinstance(samples, list):
            errors.append(f"{context}: missing {key} array")
            continue
        feed = []
        full = []
        drain_count = 0
        transition_count = 0
        previous = -1
        drained = False
        for index, sample in enumerate(samples):
            if not isinstance(sample, dict):
                errors.append(f"{context}: {key}[{index}] is not an object")
                break
            elapsed = sample.get("elapsed_ns")
            interval = sample.get("interval_ns")
            phase = sample.get("phase")
            if (not number(elapsed) or not number(interval, positive=True)
                    or phase not in ("feed", "drain", "transition") or elapsed < previous):
                errors.append(f"{context}: invalid {key}[{index}] timestamp, interval, or phase")
                break
            previous = elapsed
            full.append(interval)
            if phase in ("drain", "transition"):
                drained = True
                drain_count += phase == "drain"
                transition_count += phase == "transition"
            elif drained:
                errors.append(f"{context}: {key} returned to feed after drain")
                break
            else:
                feed.append(interval)
        result[prefix + "_feed_samples"] = len(feed)
        result[prefix + "_drain_samples"] = drain_count
        result[prefix + "_transition_samples"] = transition_count
        result[prefix + "_full_samples"] = len(full)
        if full:
            result[prefix + "_full_p95_ns"] = nearest_rank(full, 0.95)
            result[prefix + "_full_p99_ns"] = nearest_rank(full, 0.99)
        if not feed:
            errors.append(f"{context}: no feed-phase {key}")
        else:
            result[prefix + "_p95_ns"] = nearest_rank(feed, 0.95)
            result[prefix + "_p99_ns"] = nearest_rank(feed, 0.99)
    return result


def submission_metrics(raw: dict[str, Any], expected: int, cps: int, context: str,
                       errors: list[str]) -> dict[str, Any]:
    metadata, summary = raw["metadata"], raw["summary"]
    start = metadata.get("measurement_start_elapsed_ns")
    feed_end = metadata.get("feed_end_elapsed_ns")
    end = metadata.get("measurement_end_elapsed_ns")
    if not all(number(value) for value in (start, feed_end, end)) or not start < feed_end < end:
        errors.append(f"{context}: missing or unordered measurement/feed/drain timestamps")
        return {}
    for name in ("feed_offered", "feed_accepted", "feed_submitted_unique"):
        if not integer(summary.get(name)) or summary[name] > expected:
            errors.append(f"{context}: invalid {name}")
    if (integer(summary.get("feed_offered")) and integer(summary.get("feed_accepted"))
            and summary["feed_accepted"] > summary["feed_offered"]):
        errors.append(f"{context}: feed accepted count exceeds offered count")
    submissions = raw.get("submission_samples")
    if not isinstance(submissions, list):
        errors.append(f"{context}: missing exact submission_samples")
        return {}
    first: dict[int, float | int] = {}
    last = 0
    for sample in submissions:
        if not isinstance(sample, dict) or not isinstance(sample.get("comment_id"), str):
            errors.append(f"{context}: malformed submission sample")
            return {}
        identity = sample["comment_id"]
        try:
            index = int(identity.removeprefix("perf-"))
        except ValueError:
            index = -1
        at = sample.get("elapsed_ns")
        if (identity != f"perf-{index}" or not 0 <= index < expected
                or not number(at) or not start <= at <= end):
            errors.append(f"{context}: unexpected ID or submission outside fixed measurement window")
            return {}
        first[index] = min(first.get(index, at), at)
        last = max(last, at)
    if len(first) != expected:
        errors.append(f"{context}: exact submission IDs do not cover every expected comment")
        return {}
    submitted_in_feed = sum(at <= feed_end for at in first.values())
    if summary.get("feed_submitted_unique") != submitted_in_feed:
        errors.append(f"{context}: feed_submitted_unique differs from exact submission timestamps")
    if summary.get("last_submission_elapsed_ns") != last:
        errors.append(f"{context}: last_submission_elapsed_ns differs from raw submissions")
    lateness = [first[index] - start - index * 1_000_000_000 / cps
                for index in range(expected)]
    return {
        "first_draw_lateness_count": len(lateness),
        "first_draw_lateness_min_ns": min(lateness),
        "first_draw_lateness_max_ns": max(lateness),
        "first_draw_lateness_mean_ns": statistics.fmean(lateness),
        "first_draw_lateness_p50_ns": nearest_rank(lateness, 0.50),
        "first_draw_lateness_p95_ns": nearest_rank(lateness, 0.95),
        "first_draw_lateness_p99_ns": nearest_rank(lateness, 0.99),
        "first_draw_before_scheduled_wall_target": sum(value < 0 for value in lateness),
        "submitted_in_feed": submitted_in_feed,
        "submitted_in_drain": expected - submitted_in_feed,
        "feed_wall_ns": feed_end - start, "full_wall_ns": end - start,
    }


def validate_run(root: Path, run: dict[str, Any], manifest: dict[str, Any],
                 errors: list[str]) -> dict[str, Any] | None:
    context = str(run.get("run_id", "unnamed run"))
    if run.get("status") != "completed" or run.get("returncode") != 0:
        errors.append(f"{context}: execution did not complete successfully")
    relative = run.get("raw_path")
    if not isinstance(relative, str):
        errors.append(f"{context}: missing raw_path")
        return None
    path = (root / relative).resolve()
    if not path.is_relative_to(root):
        errors.append(f"{context}: raw_path escapes evidence directory")
        return None
    try:
        if sha256(path) != run.get("raw_sha256"):
            errors.append(f"{context}: raw JSON SHA256 mismatch")
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        errors.append(f"{context}: cannot read raw JSON: {error}")
        return None
    if not isinstance(raw, dict):
        errors.append(f"{context}: raw JSON must be an object")
        return None
    if raw.get("format_version") != 1 or type(raw.get("format_version")) is not int:
        errors.append(f"{context}: unsupported raw format_version")
    if raw.get("success") is not True or raw.get("errors") != []:
        errors.append(f"{context}: harness failed or has errors")
    configuration = manifest["configuration"]
    variant = manifest["variants"][run["variant"]]
    metadata = raw.get("metadata")
    if not isinstance(metadata, dict):
        errors.append(f"{context}: missing metadata object")
        metadata = {}
    for key in ("duration_ms", "tail_ms", "sample_mode"):
        if metadata.get(key) != configuration[key]:
            errors.append(f"{context}: metadata.{key} does not match planned workload")
    for key in ("cps", "text_mode"):
        if metadata.get(key) != run[key]:
            errors.append(f"{context}: metadata.{key} does not match planned case")
    for key in ("worker", "renderer"):
        if metadata.get(key) != variant[key]:
            errors.append(f"{context}: metadata.{key} does not match planned variant")
    if metadata.get("video_started") is not True:
        errors.append(f"{context}: missing evidence that the video started")
    if configuration["sample_mode"] == "timing":
        if type(metadata.get("media_start_ms")) is not int or metadata["media_start_ms"] != 0:
            errors.append(f"{context}: workload did not start at exact media zero")
        if (not number(metadata.get("media_end_ms"), positive=True)
                or metadata["media_end_ms"] < configuration["duration_ms"] + configuration["tail_ms"]):
            errors.append(f"{context}: video did not complete the fixed feed and drain window")
        if metadata.get("video_sha256") != manifest["video"]["sha256"]:
            errors.append(f"{context}: harness video SHA256 differs from the fixture")
    if configuration["sample_mode"] == "timing" and "video_path" in metadata and metadata["video_path"] != manifest["video"]["path"]:
        errors.append(f"{context}: harness used a different video path")
    for key in ("qt_version", "mpv_version", "gl_renderer"):
        if not isinstance(metadata.get(key), str) or not metadata[key]:
            errors.append(f"{context}: missing runtime identifier metadata.{key}")
    summary = raw.get("summary", {})
    if not isinstance(summary, dict):
        errors.append(f"{context}: missing summary object")
        summary = {}
    if configuration["sample_mode"] == "timing":
        expected = run["cps"] * configuration["duration_ms"] // 1000
        if not integer(summary.get("expected"), positive=True) or summary["expected"] != expected:
            errors.append(f"{context}: expected count does not match cps * duration_ms / 1000")
        for key in COUNT_EQUALS_EXPECTED:
            if not integer(summary.get(key)) or summary[key] != expected:
                errors.append(f"{context}: {key}={summary.get(key)!r}, expected {expected} (unequal/incomplete work)")
        for key in ZERO_COUNTS:
            if not integer(summary.get(key)) or summary[key] != 0:
                errors.append(f"{context}: {key}={summary.get(key)!r}, required zero")
        if summary.get("never_submitted_ids") != []:
            errors.append(f"{context}: missing or nonempty never_submitted_ids")
        snapshot = summary.get("snapshot_unique")
        if snapshot == -1:
            if not isinstance(metadata.get("snapshot_tracking"), str) or not metadata["snapshot_tracking"]:
                errors.append(f"{context}: unsampled snapshot needs an explicit tracking explanation")
        elif not integer(snapshot) or snapshot != expected:
            errors.append(f"{context}: tracked snapshot count is incomplete")
    else:
        checks = raw.get("pixel_checks")
        if (not isinstance(checks, list) or not checks
                or any(not isinstance(check, dict) or check.get("success") is not True for check in checks)):
            errors.append(f"{context}: pixel mode needs nonempty, successful pixel_checks")
    environment = run.get("environment")
    if not isinstance(environment, dict) or environment.get("QT_HASH_SEED") != "0":
        errors.append(f"{context}: QT_HASH_SEED was not fixed to 0")
    command = run.get("command", [])
    if not isinstance(command, list) or not command or command[0] != variant["path"]:
        errors.append(f"{context}: missing or mismatched executable command")
    else:
        for flag, value in (("--video", manifest["video"]["path"]), ("--cps", run["cps"]),
                            ("--text-mode", run["text_mode"]),
                            ("--duration-ms", configuration["duration_ms"]),
                            ("--tail-ms", configuration["tail_ms"]),
                            ("--sample-mode", configuration["sample_mode"]),
                            ("--worker", variant["worker"]), ("--renderer", variant["renderer"])):
            if command.count(flag) != 1 or command.index(flag) + 1 >= len(command) or command[command.index(flag) + 1] != str(value):
                errors.append(f"{context}: mismatched command argument {flag}")
    measured = (sample_metrics(raw, context, errors) if configuration["sample_mode"] == "timing"
                else {"pixel_checks": raw.get("pixel_checks", [])})
    if configuration["sample_mode"] == "timing" and isinstance(raw.get("metadata"), dict) and isinstance(raw.get("summary"), dict):
        measured.update(submission_metrics(raw, expected, run["cps"], context, errors))
    return {"run_id": context, "pair_index": run["pair_index"], "variant": run["variant"],
            "metadata": metadata, "summary": summary, **measured}


def validate_manifest(manifest: Any, errors: list[str]) -> bool:
    """Validate structural fields before the detailed evidence checks index them."""
    if not isinstance(manifest, dict):
        errors.append("manifest must be an object")
        return False
    if manifest.get("format_version") != 1 or manifest.get("kind") != "niconeon.real-render-comparison":
        errors.append("unsupported comparison manifest format")
    if manifest.get("complete") is not True:
        errors.append("campaign is incomplete; planned runs may not be omitted")
    config = manifest.get("configuration")
    if not isinstance(config, dict):
        errors.append("missing configuration")
        return False
    valid = True
    for key in ("pairs", "duration_ms", "tail_ms"):
        if not integer(config.get(key), positive=True):
            errors.append(f"invalid configuration.{key}")
            valid = False
    if (not isinstance(config.get("cps"), list) or not config["cps"]
            or not all(integer(value, positive=True) for value in config["cps"])
            or len(set(config["cps"])) != len(config["cps"])):
        errors.append("invalid cps case list")
        valid = False
    if (not isinstance(config.get("text_modes"), list) or not config["text_modes"]
            or not all(value in ("unique", "warm") for value in config["text_modes"])
            or len(set(config["text_modes"])) != len(config["text_modes"])):
        errors.append("invalid text_modes case list")
        valid = False
    if config.get("sample_mode") not in ("timing", "pixels"):
        errors.append("invalid sample_mode")
        valid = False
    variants = manifest.get("variants")
    if not isinstance(variants, dict):
        errors.append("missing variants")
        valid = False
    else:
        for name in ("baseline", "candidate"):
            variant = variants.get(name)
            if (not isinstance(variant, dict) or not isinstance(variant.get("path"), str)
                    or not valid_sha256(variant.get("sha256"))
                    or variant.get("worker") not in ("on", "off")
                    or variant.get("renderer") not in ("atlas", "frame_image")):
                errors.append(f"invalid variant {name}")
                valid = False
        if valid:
            for key in ("worker", "renderer"):
                if variants["baseline"][key] != variants["candidate"][key]:
                    errors.append(f"different {key} modes are diagnostic only, not equal-configuration acceptance")
    video = manifest.get("video")
    if (not isinstance(video, dict) or not isinstance(video.get("path"), str)
            or not valid_sha256(video.get("sha256"))):
        errors.append("missing video path or SHA256")
        valid = False
    if not isinstance(manifest.get("runs"), list):
        errors.append("missing runs array")
        valid = False
    if not isinstance(manifest.get("machine"), dict) or not manifest["machine"].get("platform"):
        errors.append("missing machine metadata")
    return valid


def analyze(manifest_path: Path, args: argparse.Namespace) -> dict[str, Any]:
    errors: list[str] = []
    report: dict[str, Any] = {
        "format_version": 1, "kind": "niconeon.real-render-analysis",
        "manifest": str(manifest_path), "status": "invalid", "errors": errors,
        "claim_scope": "frame-interval noninferiority only; not full issue acceptance",
        "warnings": [],
        "noninferiority_percent": args.noninferiority_percent,
        "minimum_pairs": args.min_pairs, "minimum_feed_samples_per_run": args.min_samples,
        "confidence_method": "paired log-ratio percentile bootstrap, pointwise two-sided 95%",
        "percentile_method": "nearest-rank within each run; separate feed and full-window sample populations",
        "limitations": LIMITATIONS.copy(), "cases": [],
    }
    try:
        report["manifest_sha256"] = sha256(manifest_path)
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        errors.append(f"cannot read manifest: {error}")
        return report
    if not validate_manifest(manifest, errors):
        return report
    root = manifest_path.parent.resolve()
    config = manifest["configuration"]
    report["video"] = manifest["video"]
    report["variants"] = manifest["variants"]
    report["machine"] = manifest["machine"]
    expected_keys = {(cps, mode, pair, variant)
                     for cps in config["cps"] for mode in config["text_modes"]
                     for pair in range(config["pairs"]) for variant in ("baseline", "candidate")}
    by_key: dict[tuple[Any, ...], dict[str, Any]] = {}
    runs_by_key = {}
    run_ids = set()
    xdg_paths: set[str] = set()
    reference_environment = None
    reference_runtime = None
    for ordinal, run in enumerate(manifest["runs"]):
        if not isinstance(run, dict):
            errors.append(f"runs[{ordinal}] must be an object")
            continue
        fields = ("cps", "text_mode", "pair_index", "variant")
        if (not integer(run.get("cps"), positive=True) or not integer(run.get("pair_index"))
                or run.get("text_mode") not in ("unique", "warm")
                or run.get("variant") not in ("baseline", "candidate")):
            errors.append(f"runs[{ordinal}] has invalid case/pair/variant")
            continue
        key = tuple(run[name] for name in fields)
        if key not in expected_keys or key in runs_by_key:
            errors.append(f"runs[{ordinal}] is an unexpected or duplicate case/pair/variant")
            continue
        if not isinstance(run.get("run_id"), str) or run["run_id"] in run_ids:
            errors.append(f"runs[{ordinal}] has missing or duplicate run_id")
        else:
            run_ids.add(run["run_id"])
        if type(run.get("ordinal")) is not int or run["ordinal"] != ordinal:
            errors.append(f"runs[{ordinal}] has inconsistent execution ordinal")
            continue
        runs_by_key[key] = run
        environment = run.get("environment")
        if reference_environment is None:
            reference_environment = environment
        elif environment != reference_environment:
            errors.append(f"runs[{ordinal}]: environment changed within the campaign")
        xdg = run.get("xdg_environment")
        required_xdg = ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "XDG_STATE_HOME")
        if not isinstance(xdg, dict):
            errors.append(f"runs[{ordinal}]: missing isolated XDG environment")
        else:
            for variable in required_xdg:
                value = xdg.get(variable)
                if not isinstance(value, str) or not value or value in xdg_paths:
                    errors.append(f"runs[{ordinal}]: {variable} is missing or reused")
                else:
                    xdg_paths.add(value)
        measured = validate_run(root, run, manifest, errors)
        if measured is not None:
            by_key[key] = measured
            runtime = {name: measured["metadata"].get(name) for name in ENVIRONMENT_METADATA}
            if reference_runtime is None:
                reference_runtime = runtime
            elif runtime != reference_runtime:
                errors.append(f"runs[{ordinal}]: runtime/font/viewport/fixture metadata changed within the campaign")
    missing = expected_keys - runs_by_key.keys()
    if missing:
        errors.append(f"missing {len(missing)} planned run(s)")
    if config["pairs"] % 2:
        errors.append("odd replicate count cannot provide exactly balanced AB/BA orders")
    for cps in config["cps"]:
        for mode in config["text_modes"]:
            orders = []
            case_result: dict[str, Any] = {"cps": cps, "text_mode": mode, "runs": [],
                                           "status": "invalid", "metrics": {}, "lateness_metrics": {}}
            report["cases"].append(case_result)
            for pair in range(config["pairs"]):
                keys = [(cps, mode, pair, name) for name in ("baseline", "candidate")]
                if not all(key in runs_by_key for key in keys):
                    continue
                a, b = (runs_by_key[key] for key in keys)
                order = "AB" if a["ordinal"] < b["ordinal"] else "BA"
                orders.append(order)
                if abs(a["ordinal"] - b["ordinal"]) != 1 or a.get("pair_order") != order or b.get("pair_order") != order:
                    errors.append(f"cps{cps}/{mode}/pair{pair}: arms were not run adjacently in recorded order")
                if a.get("environment") != b.get("environment"):
                    errors.append(f"cps{cps}/{mode}/pair{pair}: captured environments differ")
                if all(key in by_key for key in keys):
                    ma, mb = (by_key[key] for key in keys)
                    case_result["runs"].extend([ma, mb])
                    for key in ENVIRONMENT_METADATA:
                        if ma["metadata"].get(key) != mb["metadata"].get(key):
                            errors.append(f"cps{cps}/{mode}/pair{pair}: runtime metadata.{key} differs")
            if orders.count("AB") != config["pairs"] // 2 or orders.count("BA") != config["pairs"] // 2:
                errors.append(f"cps{cps}/{mode}: AB/BA order is unbalanced or incomplete")
    if errors:
        # No ratios or performance decisions are emitted for invalid work.
        return report
    if any(run["summary"].get("raster_counters_available", run["metadata"].get("raster_counters_available")) is False for run in by_key.values()):
        report["limitations"].append("one or more builds lack raster counters; identity/draw evidence remains mandatory")
    if config["sample_mode"] != "timing":
        report["status"] = "inconclusive"
        report["reason"] = "pixel-instrumented runs cannot establish timing noninferiority"
        for case in report["cases"]:
            case["status"] = "correctness_only"
        return report
    threshold = 1 + args.noninferiority_percent / 100
    report["noninferiority_ratio_threshold"] = threshold
    for case in report["cases"]:
        cps, mode = case["cps"], case["text_mode"]
        if config["pairs"] >= args.min_pairs:
            for metric in LATENESS_METRICS:
                baseline = [by_key[(cps, mode, pair, "baseline")][metric] for pair in range(config["pairs"])]
                candidate = [by_key[(cps, mode, pair, "candidate")][metric] for pair in range(config["pairs"])]
                salt = int.from_bytes(hashlib.sha256(f"{cps}:{mode}:{metric}".encode()).digest()[:8], "big")
                lateness = paired_difference_bootstrap(baseline, candidate, args.bootstrap_iterations, args.seed ^ salt)
                lateness["baseline_per_run_ns"] = baseline
                lateness["candidate_per_run_ns"] = candidate
                case["lateness_metrics"][metric] = lateness
                if lateness["status"] != "no_increase":
                    report["warnings"].append(
                        f"cps{cps}/{mode}: {metric} {lateness['status']}; review work deferral before broader qualification")
        else:
            report["warnings"].append(f"cps{cps}/{mode}: insufficient pairs for lateness CI; broader qualification unresolved")
        reasons = []
        if config["pairs"] < args.min_pairs:
            reasons.append(f"{config['pairs']} pairs, fewer than required {args.min_pairs}")
        for run in case["runs"]:
            for prefix in ("frame", "heartbeat"):
                if run[prefix + "_feed_samples"] < args.min_samples:
                    reasons.append(f"{run['run_id']}: {run[prefix + '_feed_samples']} {prefix} feed samples, fewer than {args.min_samples}")
        if reasons:
            case["status"] = "inconclusive"
            case["reasons"] = reasons
            continue
        for metric in METRICS:
            baseline = [by_key[(cps, mode, pair, "baseline")][metric] for pair in range(config["pairs"])]
            candidate = [by_key[(cps, mode, pair, "candidate")][metric] for pair in range(config["pairs"])]
            salt = int.from_bytes(hashlib.sha256(f"{cps}:{mode}:{metric}".encode()).digest()[:8], "big")
            result = paired_bootstrap(baseline, candidate, args.bootstrap_iterations, args.seed ^ salt)
            lower, upper = result["ci95_ratio"]
            result["status"] = ("noninferior" if upper <= threshold else
                                "regressed" if lower > threshold else "inconclusive")
            result["baseline_per_run_ns"] = baseline
            result["candidate_per_run_ns"] = candidate
            case["metrics"][metric] = result
        statuses = [value["status"] for value in case["metrics"].values()]
        case["status"] = ("regressed" if "regressed" in statuses else
                          "noninferior" if all(value == "noninferior" for value in statuses) else "inconclusive")
    statuses = [case["status"] for case in report["cases"]]
    report["status"] = ("regressed" if "regressed" in statuses else
                        "noninferior" if all(value == "noninferior" for value in statuses) else "inconclusive")
    return report


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("manifest", type=Path, help="manifest.json or its containing directory")
    parser.add_argument("--output", type=Path, help="write full structured analysis JSON")
    parser.add_argument("--noninferiority-percent", type=float, default=0.0,
                        help="explicit maximum regression margin; default 0 (no tolerance)")
    parser.add_argument("--min-pairs", type=int, default=10)
    parser.add_argument("--min-samples", type=int, default=1000,
                        help="minimum feed samples in EACH frame and heartbeat run (default 1000)")
    parser.add_argument("--bootstrap-iterations", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=20261001)
    args = parser.parse_args()
    if not math.isfinite(args.noninferiority_percent) or args.noninferiority_percent < 0:
        parser.error("--noninferiority-percent must be finite and nonnegative")
    if args.min_pairs < 2 or args.min_samples < 1000 or args.bootstrap_iterations < 1000:
        parser.error("need --min-pairs >= 2, --min-samples >= 1000, and --bootstrap-iterations >= 1000")
    args.manifest = args.manifest.expanduser().resolve()
    if args.manifest.is_dir():
        args.manifest /= "manifest.json"
    if args.output and args.output.expanduser().resolve().exists():
        parser.error("--output already exists; choose a new path to preserve evidence")
    return args


def main() -> int:
    args = parse_args()
    report = analyze(args.manifest, args)
    print(f"Frame-interval comparison: {report['status']}")
    print("This is not full issue acceptance; review lateness, pixels, and normal application QoS separately.")
    print(f"Explicit noninferiority margin: {args.noninferiority_percent:g}%")
    for error in report["errors"]:
        print(f"  INVALID: {error}")
    if "reason" in report:
        print(report["reason"])
    for warning in report["warnings"]:
        print(f"  WARNING: {warning}")
    for case in report["cases"]:
        print(f"cps{case['cps']} / {case['text_mode']}: {case['status']}")
        for reason in case.get("reasons", []):
            print(f"  {reason}")
        for name, result in case["metrics"].items():
            lower, upper = result["ci95_change_percent"]
            print(f"  {name}: {result['change_percent']:+.3f}% "
                  f"(95% CI {lower:+.3f}% to {upper:+.3f}%), {result['status']}")
        for name, result in case["lateness_metrics"].items():
            lower, upper = result["ci95_difference_ns"]
            print(f"  {name}: {result['mean_paired_difference_ns'] / 1e6:+.3f} ms "
                  f"(95% CI {lower / 1e6:+.3f} to {upper / 1e6:+.3f} ms), "
                  f"{result['status']} (descriptive; requires review)")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as output:
            output.write(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + "\n")
        print(f"Analysis: {args.output}")
    print("Scope: GL submission and frameSwapped timing on the recorded setup; "
          "pixel visibility, physical presentation, and normal application QoS remain separate.")
    return {"noninferior": 0, "regressed": 1, "invalid": 2, "inconclusive": 3}[report["status"]]


if __name__ == "__main__":
    raise SystemExit(main())
