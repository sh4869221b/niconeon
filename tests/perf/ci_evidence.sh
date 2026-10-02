#!/usr/bin/env bash
# One display, no concurrent compilation or benchmarks. A failed prerequisite
# keeps its raw data and cannot turn into a successful performance claim.
set -u -o pipefail
unset NICONICO_COOKIE NICONEON_NICONICO_COOKIE
root="$(pwd)"
baseline="$root/../raster-baseline/build/evidence"
candidate="$root/build/evidence"
video="$root/evidence/motion_sm9.mp4"
mode="${1:-all}"
status=0
[[ "$mode" == all || "$mode" == timing || "$mode" == quality || "$mode" == formal || "$mode" == gpu || "$mode" == cpu ]] || exit 2
if [[ "$mode" == cpu ]]; then
  mkdir -p "$root/evidence/cpu400/config" "$root/evidence/cpu400/data" "$root/evidence/cpu400/cache"
  QT_QPA_PLATFORM=xcb timeout 45s "$candidate/gl_timestamp_control" "$root/evidence/cpu400/gl-control.json" \
    > "$root/evidence/cpu400/gl-control.log" 2>&1 || status=$?
  profiler="$(ldconfig -p | awk '/libprofiler.so.0 / {print $NF; exit}')"
  profile_status=0
  if [[ -z "$profiler" ]]; then
    printf 'libprofiler unavailable\n' > "$root/evidence/cpu400/profile.log"
    status=2
  else
    # One bounded candidate diagnostic, including process startup. SIGPROF CPU
    # sampling avoids changing kernel security settings or requiring perf access.
    timeout 110s env QT_QPA_PLATFORM=xcb NICONEON_RENDER_GPU_TIMING=0 \
      XDG_CONFIG_HOME="$root/evidence/cpu400/config" XDG_DATA_HOME="$root/evidence/cpu400/data" \
      XDG_CACHE_HOME="$root/evidence/cpu400/cache" LD_PRELOAD="$profiler" \
      CPUPROFILE="$root/evidence/cpu400/candidate.prof" CPUPROFILE_FREQUENCY=100 \
      "$candidate/real_render_profile" --video "$video" \
      --output "$root/evidence/cpu400/result.json" --cps 400 --duration-ms 30000 --tail-ms 15000 \
      --text-mode unique --sample-mode timing --worker on --renderer atlas \
      > "$root/evidence/cpu400/profile.log" 2>&1 || profile_status=$?
    printf 'harness_exit=%s\n' "$profile_status" > "$root/evidence/cpu400/quality-status.txt"
    google-pprof --text --nodecount=100 "$candidate/real_render_profile" \
      "$root/evidence/cpu400/candidate.prof" > "$root/evidence/cpu400/profile-flat.txt" 2>&1 || status=$?
    google-pprof --text --cum --nodecount=100 "$candidate/real_render_profile" \
      "$root/evidence/cpu400/candidate.prof" > "$root/evidence/cpu400/profile-cumulative.txt" 2>&1 || status=$?
    # Sampling and stripped/JIT symbol limits remain explicit. These files are
    # evidence for diagnosis only; this mode never calls the acceptance analyzer.
  fi
fi
if [[ "$mode" == gpu ]]; then
  runner_status=0
  NICONEON_RENDER_GPU_TIMING=1 python3 tests/perf/run_real_render_comparison.py \
    --baseline "$baseline/real_render_profile" --candidate "$candidate/real_render_profile" \
    --video "$video" --output-dir "$root/evidence/gpu400" \
    --pairs 2 --seed 20261002 --cps 400 --text-modes unique \
    --duration-ms 30000 --tail-ms 15000 --keep-going || runner_status=$?
  printf 'runner_exit=%s\n' "$runner_status" > "$root/evidence/gpu-runner-status.txt"
  # Quality failures stay visible; only timestamp coverage is this diagnostic gate.
  python3 tests/perf/summarize_gpu_probe.py "$root/evidence/gpu400" || status=$?
fi
if [[ "$mode" == formal ]]; then
  runner_status=0
  python3 tests/perf/run_real_render_comparison.py \
    --baseline "$baseline/real_render_profile" --candidate "$candidate/real_render_profile" \
    --video "$video" --output-dir "$root/evidence/formal100" \
    --pairs 10 --seed 20261002 --cps 100 --text-modes unique \
    --duration-ms 180000 --tail-ms 15000 --keep-going || runner_status=$?
  printf 'runner_exit=%s\n' "$runner_status" > "$root/evidence/formal-runner-status.txt"
  # Raw baseline quality failures remain in the manifest. Only the explicit,
  # narrowly validated historical transient-image case can be separated here.
  python3 tests/perf/analyze_real_render.py "$root/evidence/formal100" \
    --baseline-transient-missing --min-pairs 10 --min-samples 1000 \
    --noninferiority-percent 0 --bootstrap-iterations 10000 --seed 20261002 \
    --output "$root/evidence/formal100/analysis.json" || status=$?
fi
if [[ "$mode" == timing || "$mode" == all ]]; then
# Discovery pass only: two balanced AB/BA pairs, not a statistical acceptance claim.
python3 tests/perf/run_real_render_comparison.py \
  --baseline "$baseline/real_render_profile" --candidate "$candidate/real_render_profile" \
  --video "$video" --output-dir "$root/evidence/equal-work" \
  --pairs 2 --cps 100 200 400 --text-modes unique --duration-ms 30000 --tail-ms 15000 \
  --keep-going || status=1
# Keep the normal application's adaptive QoS and source policy unchanged.
# Balanced B-A / A-B order; separate process and settings for each trial.
for entry in candidate-1 baseline-1 baseline-2 candidate-2; do
  arm="${entry%-*}"
  binary="$candidate/niconeon"
  [[ "$arm" == baseline ]] && binary="$baseline/niconeon"
  run="$root/evidence/qos/$entry"
  mkdir -p "$run/config/sh4869221b" "$run/data" "$run/cache"
  cat > "$run/config/sh4869221b/Niconeon.conf" <<'SETTINGS'
[ui]
perfProfile=high
targetFps=60
maxEmitPerTick=0
coalesceSameContent=false
commentsVisible=true
perfLogEnabled=true
SETTINGS
  env XDG_CONFIG_HOME="$run/config" XDG_DATA_HOME="$run/data" XDG_CACHE_HOME="$run/cache" \
    QT_QPA_PLATFORM=xcb QT_HASH_SEED=0 NICONEON_RENDER_DIAGNOSTICS=1 \
    NICONEON_APP_PROFILE_OUTPUT="$run/result.json" \
    NICONEON_AUTO_VIDEO_PATH="$video" NICONEON_AUTO_PERF_LOG=1 NICONEON_AUTO_EXIT_MS=48000 \
    NICONEON_SYNTHETIC_COMMENTS=ramp NICONEON_SYNTHETIC_DURATION_SEC=30 \
    NICONEON_SYNTHETIC_BASE_PER_SEC=400 NICONEON_SYNTHETIC_RAMP_PER_SEC=0 NICONEON_SYNTHETIC_MAX_PER_SEC=400 \
    timeout 70s "$binary" > "$run/run.log" 2>&1 || status=1
done
fi
if [[ "$mode" == quality || "$mode" == all ]]; then
# Keep failures from each independent correctness scenario in its own artifact.
for dpr in 1 1.5 2; do
  for suite in basic wide atlas-pressure active-capacity appearance; do
    # Preserve the established finite-pressure counts at1/2; extend mapping/seam coverage only.
    [[ "$dpr" == 1.5 && ( "$suite" == atlas-pressure || "$suite" == active-capacity ) ]] && continue
    run="$root/evidence/pixel-candidate-$suite-dpr$dpr"
    QT_QPA_PLATFORM=xcb QT_SCALE_FACTOR="$dpr" timeout 150s "$candidate/real_render_profile" \
      --sample-mode pixels --pixel-suite "$suite" --expected-dpr "$dpr" --output "$run.json" > "$run.log" 2>&1 || status=1
  done
done
QT_QPA_PLATFORM=xcb QT_SCALE_FACTOR=1 timeout 90s "$baseline/real_render_profile" \
  --sample-mode pixels --pixel-suite basic --expected-dpr 1 --output "$root/evidence/pixel-baseline.json" \
  > "$root/evidence/pixel-baseline.log" 2>&1 || status=1
fi
printf 'measurement_exit=%s\n' "$status" > "$root/evidence/$mode-status.txt"
exit "$status"
