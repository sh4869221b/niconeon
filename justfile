set windows-shell := ["cmd.exe", "/d", "/c"]

bazel := "bazelisk"

default:
    @just --list

core-test:
    {{bazel}} test //core/...

licenses:
    scripts/release/generate_third_party_notices.sh

license-check: licenses
    git diff --exit-code -- THIRD_PARTY_NOTICES.txt

core-build:
    {{bazel}} build //core:niconeon-core

ui-configure:
    {{bazel}} cquery //app-ui:niconeon-ui >/dev/null

ui-build:
    {{bazel}} build //app-ui:niconeon-ui

ui-test:
    {{bazel}} test //app-ui:ui_unit_tests

ui-e2e:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -n "${DISPLAY:-}" ]; then
      {{bazel}} test \
        --test_output=errors \
        --test_env=DISPLAY \
        --test_env=LIBGL_ALWAYS_SOFTWARE \
        --test_env=MESA_LOADER_DRIVER_OVERRIDE \
        //app-ui:rendernode_alignment_e2e
    elif [ -n "${WAYLAND_DISPLAY:-}" ]; then
      {{bazel}} test \
        --test_output=errors \
        --test_env=WAYLAND_DISPLAY \
        --test_env=LIBGL_ALWAYS_SOFTWARE \
        --test_env=MESA_LOADER_DRIVER_OVERRIDE \
        //app-ui:rendernode_alignment_e2e
    elif command -v xvfb-run >/dev/null 2>&1; then
      xvfb-run -a -s "-screen 0 1280x1024x24 -ac" env \
        LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}" \
        MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}" \
        NICONEON_DANMAKU_RENDERER="${NICONEON_DANMAKU_RENDERER:-frame_image}" \
        {{bazel}} test \
          --test_output=errors \
          --test_env=DISPLAY \
          --test_env=LIBGL_ALWAYS_SOFTWARE \
          --test_env=MESA_LOADER_DRIVER_OVERRIDE \
          --test_env=NICONEON_DANMAKU_RENDERER \
          //app-ui:rendernode_alignment_e2e
    else
      echo "DISPLAY/WAYLAND_DISPLAY is not set and xvfb-run is unavailable; install xvfb-run or run from a desktop session." >&2
      exit 1
    fi

build: licenses core-build ui-build

run: build
    #!/usr/bin/env bash
    set -euo pipefail
    bazel_file() {
      {{bazel}} cquery --output=files "$1" 2>/dev/null | tail -n1
    }
    abs_file() {
      case "$1" in
        /*) printf '%s\n' "$1" ;;
        *) printf '%s/%s\n' "$PWD" "$1" ;;
      esac
    }
    core_bin="$(abs_file "$(bazel_file //core:niconeon-core)")"
    ui_bin="$(abs_file "$(bazel_file //app-ui:niconeon-ui)")"
    NICONEON_CORE_BIN="$core_bin" "$ui_bin"

perf-dummy out="perf-dummy.log" duration="60": build
    scripts/perf/run_dummy_profile.sh {{out}} {{duration}}

clean:
    #!/usr/bin/env bash
    set -euo pipefail
    {{bazel}} clean
    for path in bazel-bin bazel-out bazel-testlogs "bazel-$(basename "$PWD")"; do
      if [ -L "$path" ]; then
        rm "$path"
      fi
    done
