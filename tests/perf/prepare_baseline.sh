#!/usr/bin/env bash
# Prepare the fixed pre-worker baseline with observer-only changes and identical
# benchmark source. Never copy the candidate renderer or behavior into this arm.
set -euo pipefail
[[ $# == 1 ]] || { echo 'usage: prepare_baseline.sh NEW_WORKTREE_PATH' >&2; exit 2; }
root="$(git rev-parse --show-toplevel)"
base=3141e66054b01c1a960c0c5a4b4d271b4f8209a5
[[ ! -e "$1" ]] || { echo 'baseline path already exists' >&2; exit 2; }
git -C "$root" worktree add --detach "$1" "$base"
target="$(cd "$1" && pwd)"
git -C "$target" apply "$root/tests/perf/baseline-observer.patch"
mkdir -p "$target/tests/perf"
cp "$root/tests/perf/real_render_profile.cpp" "$root/tests/perf/app_profile.hpp" "$root/tests/perf/frame_phases.hpp" "$root/tests/perf/comment_timing_json.hpp" "$root/tests/perf/bilinear_reference.hpp" "$target/tests/perf/"
cat >> "$target/CMakeLists.txt" <<'CMAKE'

# Observer-only test overlay, copied identically from the comparison candidate.
option(NICONEON_BUILD_PERF_TOOLS "Build real-render profiler" OFF)
if(NICONEON_BUILD_PERF_TOOLS AND NICONEON_BUILD_APP)
  target_compile_definitions(niconeon PRIVATE NICONEON_APP_PROFILE=1)
  target_include_directories(niconeon PRIVATE tests/perf)
  qt_add_executable(real_render_profile tests/perf/real_render_profile.cpp)
  target_link_libraries(real_render_profile PRIVATE niconeon_render niconeon_playback Qt6::Qml)
  set_target_properties(real_render_profile PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
endif()
CMAKE
printf 'baseline=%s\nobserver_sha256=' "$base"
sha256sum "$root/tests/perf/baseline-observer.patch"
