#!/usr/bin/env python3
"""Run randomized, paired real-render workloads and retain reproducible evidence.

Example (use a real OpenGL display, or explicitly wrap this in xvfb-run):
  python3 tests/perf/run_real_render_comparison.py \
      --baseline /path/baseline/real_render_profile \
      --candidate /path/candidate/real_render_profile \
      --video /path/fixture.mp4 --output-dir /tmp/render-comparison

The same executable can be supplied twice to compare explicit worker/renderer
settings. No application-QoS, hardware-GPU, or pixel-correctness claim follows
from a timing run. Pixel runs must be collected separately and are not timing
qualification. This script does not build binaries or change display settings.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import subprocess
import sys
import time
from typing import Any

FORMAT_VERSION = 1
# Deliberately do not serialize the complete environment (it may contain secrets).
ENVIRONMENT_KEYS = (
    "DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE", "LANG", "LC_ALL", "LC_NUMERIC", "TZ",
    "FONTCONFIG_FILE", "FONTCONFIG_PATH",
    "QT_QPA_PLATFORM", "QT_QPA_PLATFORM_PLUGIN_PATH", "QT_PLUGIN_PATH",
    "QT_QUICK_BACKEND", "QSG_RHI_BACKEND", "QSG_RENDER_LOOP", "QSG_INFO",
    "QSG_RHI_DEBUG_LAYER", "QSG_RHI_PROFILE", "QT_XCB_GL_INTEGRATION",
    "QT_OPENGL", "QT_SCALE_FACTOR", "QT_AUTO_SCREEN_SCALE_FACTOR",
    "QT_FONT_DPI", "QT_ENABLE_HIGHDPI_SCALING", "QT_SCALE_FACTOR_ROUNDING_POLICY",
    "QT_NO_GLIB", "LIBGL_ALWAYS_SOFTWARE", "LIBGL_DRIVERS_PATH", "GALLIUM_DRIVER",
    "MESA_LOADER_DRIVER_OVERRIDE", "MESA_GL_VERSION_OVERRIDE", "DRI_PRIME",
    "__GL_SYNC_TO_VBLANK", "vblank_mode", "LP_NUM_THREADS",
    "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "QT_HASH_SEED",
    "NICONEON_MPV_SCALE", "NICONEON_MPV_AO", "NICONEON_SIMD_MODE",
    "NICONEON_RENDER_DIAGNOSTICS",
    "NICONEON_DANMAKU_WORKER", "NICONEON_DANMAKU_RENDERER",
)
REMOVED_ENVIRONMENT_KEYS = (
    "NICONEON_AUTO_VIDEO_PATH", "NICONEON_SYNTHETIC_COMMENTS",
    "NICONEON_AUTO_PERF_LOG", "NICONEON_AUTO_EXIT_MS",
    "NICONEON_NICONICO_COOKIE", "NICONICO_COOKIE",
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: Any) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False,
                                    allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def positive_integer(value: str) -> int:
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--video", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="new directory; existing evidence is never overwritten")
    parser.add_argument("--pairs", type=positive_integer, default=10,
                        help="pairs per case, even for exactly balanced AB/BA (default: 10)")
    parser.add_argument("--seed", type=int, default=20261001)
    parser.add_argument("--cps", type=positive_integer, nargs="+", default=[100, 200, 400])
    parser.add_argument("--text-modes", choices=("unique", "warm"), nargs="+",
                        default=["unique", "warm"])
    parser.add_argument("--duration-ms", type=positive_integer, default=30000)
    parser.add_argument("--tail-ms", type=positive_integer, default=15000)
    parser.add_argument("--sample-mode", choices=("timing", "pixels"), default="timing")
    parser.add_argument("--baseline-worker", choices=("on", "off"), default="on")
    parser.add_argument("--candidate-worker", choices=("on", "off"), default="on")
    parser.add_argument("--baseline-renderer", choices=("atlas", "frame_image"), default="atlas")
    parser.add_argument("--candidate-renderer", choices=("atlas", "frame_image"), default="atlas")
    parser.add_argument("--baseline-label", default="baseline")
    parser.add_argument("--candidate-label", default="candidate")
    parser.add_argument("--cooldown-seconds", type=float, default=1.0)
    parser.add_argument("--timeout-seconds", type=float,
                        help="per run; default is twice (feed + drain) + 40 seconds")
    parser.add_argument("--keep-going", action="store_true",
                        help="retain all planned runs even after a failed harness run")
    args = parser.parse_args()
    if args.pairs % 2:
        parser.error("--pairs must be even for balanced AB/BA order")
    if len(set(args.cps)) != len(args.cps) or len(set(args.text_modes)) != len(args.text_modes):
        parser.error("duplicate cases are not allowed")
    if (not 1000 <= args.duration_ms <= 120000 or not 1000 <= args.tail_ms <= 60000
            or any(cps > 2000 or cps * args.duration_ms // 1000 > 60000 for cps in args.cps)):
        parser.error("harness bounds: duration 1000..120000ms, tail 1000..60000ms, cps <= 2000, <= 60000 comments")
    if args.cooldown_seconds < 0 or not args.cooldown_seconds < float("inf"):
        parser.error("--cooldown-seconds must be finite and nonnegative")
    if args.timeout_seconds is None:
        args.timeout_seconds = 2 * (args.duration_ms + args.tail_ms) / 1000 + 40
    if not 0 < args.timeout_seconds < float("inf"):
        parser.error("--timeout-seconds must be finite and positive")
    for name in ("baseline", "candidate", "video"):
        path = getattr(args, name).expanduser().resolve()
        if not path.is_file():
            parser.error(f"--{name} is not a regular file: {path}")
        if name != "video" and not os.access(path, os.X_OK):
            parser.error(f"--{name} is not executable: {path}")
        setattr(args, name, path)
    args.output_dir = args.output_dir.expanduser().resolve()
    if args.output_dir.exists():
        parser.error("--output-dir already exists; choose a new directory")
    return args


def environment_metadata(environment: dict[str, str]) -> dict[str, str | None]:
    return {name: environment.get(name) for name in ENVIRONMENT_KEYS}


def machine_metadata() -> dict[str, Any]:
    metadata: dict[str, Any] = {
        "platform": platform.platform(), "machine": platform.machine(),
        "processor": platform.processor(), "python": sys.version,
        "logical_cpu_count": os.cpu_count(),
    }
    try:
        metadata["cpu_affinity"] = sorted(os.sched_getaffinity(0))
    except AttributeError:
        pass
    try:
        metadata["process_nice"] = os.getpriority(os.PRIO_PROCESS, 0)
    except (AttributeError, OSError):
        pass
    # No hostname, credentials, or complete environment dump.
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        for line in cpuinfo.read_text(encoding="utf-8").splitlines():
            if line.startswith("model name"):
                metadata["cpu_model"] = line.partition(":")[2].strip()
                break
    return metadata


def plan_runs(args: argparse.Namespace) -> list[dict[str, Any]]:
    rng = random.Random(args.seed)
    cases = [(cps, mode) for cps in args.cps for mode in args.text_modes]
    orders = {}
    for case in cases:
        values = ["AB"] * (args.pairs // 2) + ["BA"] * (args.pairs // 2)
        rng.shuffle(values)
        orders[case] = values
    result = []
    # Each replicate is a randomized complete block; paired arms stay adjacent.
    for pair_index in range(args.pairs):
        block = cases.copy()
        rng.shuffle(block)
        for cps, mode in block:
            order = orders[(cps, mode)][pair_index]
            for arm in order:
                variant = "baseline" if arm == "A" else "candidate"
                run_id = f"{len(result) + 1:04d}-cps{cps}-{mode}-p{pair_index + 1:02d}-{variant}"
                result.append({
                    "run_id": run_id, "ordinal": len(result), "pair_index": pair_index,
                    "cps": cps, "text_mode": mode, "variant": variant,
                    "pair_order": order, "status": "planned",
                })
    return result


def run_one(args: argparse.Namespace, manifest: dict[str, Any],
            run: dict[str, Any], base_environment: dict[str, str]) -> None:
    variant = manifest["variants"][run["variant"]]
    executable = Path(variant["path"])
    # Abort if an in-place rebuild or fixture edit would mix evidence versions.
    if sha256(executable) != variant["sha256"]:
        raise RuntimeError(f"executable changed during campaign: {executable}")
    if sha256(args.video) != manifest["video"]["sha256"]:
        raise RuntimeError("video changed during campaign")
    directory = args.output_dir / "runs" / run["run_id"]
    directory.mkdir(parents=True)
    environment = base_environment.copy()
    for variable, name in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                           ("XDG_CACHE_HOME", "cache"), ("XDG_RUNTIME_DIR", "runtime"),
                           ("XDG_STATE_HOME", "state")):
        path = directory / "xdg" / name
        path.mkdir(parents=True, mode=0o700)
        environment[variable] = str(path)
    environment["NICONEON_DANMAKU_WORKER"] = variant["worker"]
    environment["NICONEON_DANMAKU_RENDERER"] = variant["renderer"]
    raw_path = directory / "raw.json"
    command = [str(executable), "--video", str(args.video), "--output", str(raw_path),
               "--cps", str(run["cps"]), "--duration-ms", str(args.duration_ms),
               "--tail-ms", str(args.tail_ms), "--text-mode", run["text_mode"],
               "--sample-mode", args.sample_mode, "--worker", variant["worker"],
               "--renderer", variant["renderer"]]
    run.update({
        "status": "running", "started_utc": utc_now(), "command": command,
        "working_directory": str(directory),
        "environment": environment_metadata(environment),
        "xdg_environment": {key: value for key, value in environment.items()
                            if key in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME",
                                       "XDG_RUNTIME_DIR", "XDG_STATE_HOME")},
        "raw_path": str(raw_path.relative_to(args.output_dir)),
        "stdout_path": str((directory / "stdout.log").relative_to(args.output_dir)),
        "stderr_path": str((directory / "stderr.log").relative_to(args.output_dir)),
    })
    write_json(args.output_dir / "manifest.json", manifest)
    started = time.monotonic()
    failure = None
    with (directory / "stdout.log").open("wb") as stdout, (directory / "stderr.log").open("wb") as stderr:
        try:
            completed = subprocess.run(command, cwd=directory, env=environment,
                                       stdout=stdout, stderr=stderr,
                                       timeout=args.timeout_seconds, check=False)
            run["returncode"] = completed.returncode
            if completed.returncode:
                failure = f"harness exited {completed.returncode}"
        except subprocess.TimeoutExpired:
            run["returncode"] = None
            failure = f"harness timed out after {args.timeout_seconds:g} seconds"
        except OSError as error:
            run["returncode"] = None
            failure = str(error)
    run["wall_seconds"] = time.monotonic() - started
    run["finished_utc"] = utc_now()
    if raw_path.is_file():
        run["raw_sha256"] = sha256(raw_path)
        try:
            raw = json.loads(raw_path.read_text(encoding="utf-8"))
            if not isinstance(raw, dict) or raw.get("success") is not True:
                failure = failure or "harness did not report success=true"
        except (ValueError, OSError) as error:
            failure = failure or f"invalid raw JSON: {error}"
    else:
        failure = failure or "harness did not create raw JSON"
    run["status"] = "failed" if failure else "completed"
    if failure:
        run["failure"] = failure
    write_json(args.output_dir / "manifest.json", manifest)


def main() -> int:
    args = parse_args()
    environment = os.environ.copy()
    for key in REMOVED_ENVIRONMENT_KEYS:
        environment.pop(key, None)
    environment["QT_HASH_SEED"] = "0"
    # Wayland normally resolves a relative socket in XDG_RUNTIME_DIR. Preserve
    # the display connection while still giving each run its own runtime home.
    wayland = environment.get("WAYLAND_DISPLAY")
    runtime = environment.get("XDG_RUNTIME_DIR")
    if wayland and runtime and not os.path.isabs(wayland):
        environment["WAYLAND_DISPLAY"] = str(Path(runtime) / wayland)
    args.output_dir.mkdir(parents=True)
    manifest: dict[str, Any] = {
        "format_version": FORMAT_VERSION, "kind": "niconeon.real-render-comparison",
        "created_utc": utc_now(), "complete": False,
        "runner_sha256": sha256(Path(__file__)),
        "configuration": {
            "pairs": args.pairs, "seed": args.seed, "cps": args.cps,
            "text_modes": args.text_modes, "duration_ms": args.duration_ms,
            "tail_ms": args.tail_ms, "sample_mode": args.sample_mode,
            "cooldown_seconds": args.cooldown_seconds,
            "timeout_seconds": args.timeout_seconds,
            "ordering": "randomized complete blocks; paired adjacent; balanced AB/BA per case",
        },
        "video": {"path": str(args.video), "sha256": sha256(args.video),
                  "size_bytes": args.video.stat().st_size},
        "machine": machine_metadata(), "environment": environment_metadata(environment),
        "removed_environment_keys": list(REMOVED_ENVIRONMENT_KEYS),
        "variants": {}, "runs": plan_runs(args),
    }
    for name in ("baseline", "candidate"):
        executable = getattr(args, name)
        manifest["variants"][name] = {
            "path": str(executable), "sha256": sha256(executable),
            "label": getattr(args, name + "_label"),
            "worker": getattr(args, name + "_worker"),
            "renderer": getattr(args, name + "_renderer"),
        }
    manifest_path = args.output_dir / "manifest.json"
    write_json(manifest_path, manifest)
    exit_code = 0
    try:
        for index, run in enumerate(manifest["runs"]):
            if index and args.cooldown_seconds:
                time.sleep(args.cooldown_seconds)
            print(f"[{index + 1}/{len(manifest['runs'])}] {run['run_id']}", flush=True)
            run_one(args, manifest, run, environment)
            if run["status"] != "completed":
                print(f"  FAILED: {run['failure']}", file=sys.stderr, flush=True)
                exit_code = 1
                if not args.keep_going:
                    break
        manifest["complete"] = all(run["status"] in ("completed", "failed")
                                   for run in manifest["runs"])
    except KeyboardInterrupt:
        manifest["interrupted"] = True
        exit_code = 130
    except (OSError, RuntimeError) as error:
        manifest["runner_error"] = str(error)
        print(str(error), file=sys.stderr)
        exit_code = 1
    finally:
        manifest["finished_utc"] = utc_now()
        write_json(manifest_path, manifest)
    print(f"Evidence: {manifest_path}")
    print("Execution finished; use analyze_real_render.py before any performance claim.")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
