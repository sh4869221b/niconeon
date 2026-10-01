#!/usr/bin/env bash
# One display, no concurrent compilation or benchmarks. A failed prerequisite
# keeps its raw data and cannot turn into a successful performance claim.
set -u -o pipefail
root="$(pwd)"
baseline="$root/../raster-baseline/build/evidence"
candidate="$root/build/evidence"
video="$root/evidence/motion_sm9.mp4"
status=0
for arm in baseline candidate; do
  binary="$candidate/real_render_profile"
  [[ "$arm" == baseline ]] && binary="$baseline/real_render_profile"
  QT_QPA_PLATFORM=xcb timeout 90s "$binary" --sample-mode pixels --output "$root/evidence/pixel-$arm.json" > "$root/evidence/pixel-$arm.log" 2>&1 || status=1
done
# Discovery pass only: two balanced AB/BA pairs, not a statistical acceptance claim.
python3 tests/perf/run_real_render_comparison.py \
  --baseline "$baseline/real_render_profile" --candidate "$candidate/real_render_profile" \
  --video "$video" --output-dir "$root/evidence/equal-work" \
  --pairs 2 --cps 400 --text-modes unique --duration-ms 30000 --tail-ms 15000 \
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
printf 'measurement_exit=%s\n' "$status" > "$root/evidence/measurement-status.txt"
exit "$status"
