#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_LOG="${1:-$ROOT_DIR/perf-dummy.log}"
DURATION_SEC="${2:-60}"

if ! [[ "$DURATION_SEC" =~ ^[0-9]+$ ]] || [ "$DURATION_SEC" -le 0 ]; then
  echo "duration must be positive integer seconds: $DURATION_SEC" >&2
  exit 1
fi

CORE_BIN="${NICONEON_CORE_BIN:-$ROOT_DIR/core/target/debug/niconeon-core}"
UI_BIN="${NICONEON_UI_BIN:-$ROOT_DIR/app-ui/build/niconeon-ui}"
VIDEO_PATH="${NICONEON_DUMMY_VIDEO_PATH:-/tmp/niconeon-sm9-dummy.mp4}"

if [ ! -x "$CORE_BIN" ]; then
  echo "core binary not found: $CORE_BIN" >&2
  exit 1
fi
if [ ! -x "$UI_BIN" ]; then
  echo "ui binary not found: $UI_BIN" >&2
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
RUNNER=()
DEFAULT_SOFT_GL=0
if [ -z "${DISPLAY:-}" ]; then
  if command -v xvfb-run >/dev/null 2>&1; then
    RUNNER=(xvfb-run -a)
    DEFAULT_SOFT_GL=1
  else
    echo "DISPLAY is not set. install xvfb and retry (or run in desktop session)." >&2
    exit 1
  fi
fi

mkdir -p "$(dirname "$OUT_LOG")"
"${RUNNER[@]}" env \
  LC_NUMERIC=C \
  LIBGL_ALWAYS_SOFTWARE="${NICONEON_LIBGL_ALWAYS_SOFTWARE:-$DEFAULT_SOFT_GL}" \
  MESA_LOADER_DRIVER_OVERRIDE="${NICONEON_MESA_DRIVER_OVERRIDE:-llvmpipe}" \
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

echo "wrote profile log: $OUT_LOG"
