#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_LOG="${1:-$ROOT_DIR/perf-dummy.log}"
DURATION_SEC="${2:-60}"

if ! [[ "$DURATION_SEC" =~ ^[0-9]+$ ]] || [ "$DURATION_SEC" -le 0 ]; then
  echo "duration must be positive integer seconds: $DURATION_SEC" >&2
  exit 1
fi

bazel_output() {
  local target="$1"
  local output
  output="$(cd "$ROOT_DIR" && bazelisk cquery --output=files "$target" 2>/dev/null | tail -n1)"
  if [ -z "$output" ]; then
    echo "Bazel output discovery failed for $target" >&2
    exit 1
  fi
  case "$output" in
    /*) printf '%s\n' "$output" ;;
    *) printf '%s/%s\n' "$ROOT_DIR" "$output" ;;
  esac
}

CORE_BIN="${NICONEON_CORE_BIN:-$(bazel_output //core:niconeon-core)}"
UI_BIN="${NICONEON_UI_BIN:-$(bazel_output //app-ui:niconeon-ui)}"
VIDEO_PATH="${NICONEON_DUMMY_VIDEO_PATH:-/tmp/niconeon-sm9-dummy.mp4}"

if [ ! -x "$CORE_BIN" ]; then
  echo "core binary not found: $CORE_BIN; run just build or set NICONEON_CORE_BIN" >&2
  exit 1
fi
if [ ! -x "$UI_BIN" ]; then
  echo "ui binary not found: $UI_BIN; run just build or set NICONEON_UI_BIN" >&2
  exit 1
fi
if ! [[ "$(basename "$VIDEO_PATH")" =~ (sm|nm|so)[0-9]+ ]]; then
  echo "dummy video filename must include video id pattern (sm|nm|so + digits): $VIDEO_PATH" >&2
  exit 1
fi

if [ "${NICONEON_DUMMY_REGEN:-0}" = "1" ] || [ ! -f "$VIDEO_PATH" ]; then
  if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "ffmpeg is required to generate dummy video" >&2
    exit 1
  fi
  mkdir -p "$(dirname "$VIDEO_PATH")"
  ffmpeg -y \
    -f lavfi -i "testsrc2=size=1280x720:rate=30" \
    -f lavfi -i "anullsrc=r=48000:cl=stereo" \
    -t "$DURATION_SEC" \
    -shortest \
    -c:v libx264 -pix_fmt yuv420p -preset veryfast \
    -c:a aac \
    -movflags +faststart \
    "$VIDEO_PATH" >/dev/null 2>&1
fi

AUTO_EXIT_MS=$((DURATION_SEC * 1000 + 4000))

mkdir -p "$(dirname "$OUT_LOG")"
run_profile() {
  "$@" env \
  LC_NUMERIC=C \
  LIBGL_ALWAYS_SOFTWARE="${NICONEON_LIBGL_ALWAYS_SOFTWARE:-0}" \
  MESA_LOADER_DRIVER_OVERRIDE="${NICONEON_MESA_DRIVER_OVERRIDE:-llvmpipe}" \
  NICONEON_MPV_AO="${NICONEON_MPV_AO:-}" \
  NICONEON_CORE_BIN="$CORE_BIN" \
  NICONEON_AUTO_VIDEO_PATH="$VIDEO_PATH" \
  NICONEON_AUTO_PERF_LOG=1 \
  NICONEON_AUTO_EXIT_MS="$AUTO_EXIT_MS" \
  NICONEON_SYNTHETIC_COMMENTS=ramp \
  NICONEON_SYNTHETIC_DURATION_SEC="$DURATION_SEC" \
  NICONEON_SYNTHETIC_BASE_PER_SEC="${NICONEON_SYNTHETIC_BASE_PER_SEC:-1}" \
  NICONEON_SYNTHETIC_RAMP_PER_SEC="${NICONEON_SYNTHETIC_RAMP_PER_SEC:-1}" \
  NICONEON_SYNTHETIC_MAX_PER_SEC="${NICONEON_SYNTHETIC_MAX_PER_SEC:-160}" \
  NICONEON_SYNTHETIC_USER_SPAN="${NICONEON_SYNTHETIC_USER_SPAN:-200}" \
  "$UI_BIN" 2>&1 | tee "$OUT_LOG"
}

has_perf_markers() {
  grep -Eq '^\[perf-ui\]' "$OUT_LOG" \
    && grep -Eq '^\[perf-danmaku\]' "$OUT_LOG" \
    && grep -Eq '^\[perf-render\]' "$OUT_LOG"
}

if [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then
  run_profile
  if ! has_perf_markers; then
    echo "profile run completed without perf markers: $OUT_LOG" >&2
    exit 1
  fi
  echo "wrote profile log: $OUT_LOG"
  exit 0
fi

TMP_LOG="$(mktemp "${OUT_LOG}.offscreen.XXXXXX")"
cleanup() {
  rm -f "$TMP_LOG"
}
trap cleanup EXIT

if QT_QPA_PLATFORM=offscreen \
  NICONEON_LIBGL_ALWAYS_SOFTWARE="${NICONEON_LIBGL_ALWAYS_SOFTWARE:-1}" \
  NICONEON_MPV_AO="${NICONEON_MPV_AO:-null}" \
  run_profile; then
  if has_perf_markers; then
    echo "wrote profile log: $OUT_LOG"
    exit 0
  fi
  cp "$OUT_LOG" "$TMP_LOG"
  echo "offscreen run completed without perf markers; retrying with xvfb-run if available" >&2
else
  cp "$OUT_LOG" "$TMP_LOG" 2>/dev/null || true
  echo "offscreen run failed; retrying with xvfb-run if available" >&2
fi

if ! command -v xvfb-run >/dev/null 2>&1; then
  {
    echo "deterministic environment constraint: DISPLAY and WAYLAND_DISPLAY are unset, xvfb-run is unavailable, and the offscreen run did not emit required perf markers."
    cat "$TMP_LOG"
  } | tee "$OUT_LOG" >&2
  if [ "${NICONEON_ALLOW_HEADLESS_PERF_CONSTRAINT:-0}" = "1" ]; then
    echo "headless perf constraint accepted because NICONEON_ALLOW_HEADLESS_PERF_CONSTRAINT=1" | tee -a "$OUT_LOG" >&2
    exit 0
  fi
  echo "set NICONEON_ALLOW_HEADLESS_PERF_CONSTRAINT=1 only when recording constrained headless evidence intentionally" | tee -a "$OUT_LOG" >&2
  exit 1
fi

xvfb-run -a env \
  QT_QPA_PLATFORM="${NICONEON_QPA_PLATFORM_UNDER_XVFB:-xcb}" \
  NICONEON_LIBGL_ALWAYS_SOFTWARE="${NICONEON_LIBGL_ALWAYS_SOFTWARE:-1}" \
  NICONEON_MPV_AO="${NICONEON_MPV_AO:-null}" \
  bash -lc "exec \"\$0\" \"\$@\"" "$0" "$OUT_LOG" "$DURATION_SEC"

echo "wrote profile log: $OUT_LOG"
