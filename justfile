set windows-shell := ["cmd.exe", "/d", "/c"]

core_bin := if os() == "windows" {
  "core\\target\\debug\\niconeon-core.exe"
} else {
  "core/target/debug/niconeon-core"
}

ui_bin := if os() == "windows" {
  "app-ui\\build\\niconeon-ui.exe"
} else {
  "app-ui/build/niconeon-ui"
}

default:
    @just --list

core-test:
    cd core && cargo test

licenses:
    scripts/release/generate_third_party_notices.sh

license-check: licenses
    git diff --exit-code -- THIRD_PARTY_NOTICES.txt

core-build:
    cd core && cargo build -p niconeon-core

ui-configure:
    cd app-ui && cmake -S . -B build

ui-build:
    cd app-ui && cmake --build build -j

ui-e2e:
    cd app-ui && cmake -S . -B build-e2e -DBUILD_TESTING=ON -DNICONEON_BUILD_UI_E2E=ON
    cd app-ui && cmake --build build-e2e -j
    cd app-ui && if [ -n "${DISPLAY:-}" ]; then ctest --test-dir build-e2e --output-on-failure -R rendernode_alignment_e2e; elif command -v xvfb-run >/dev/null 2>&1; then xvfb-run -a ctest --test-dir build-e2e --output-on-failure -R rendernode_alignment_e2e; else echo "DISPLAY is not set. install xvfb-run and retry." >&2; exit 1; fi

build: licenses core-build ui-configure ui-build

run: build
    {{ if os() == "windows" { "set NICONEON_CORE_BIN=core\\target\\debug\\niconeon-core.exe && app-ui\\build\\niconeon-ui.exe" } else { "NICONEON_CORE_BIN=core/target/debug/niconeon-core app-ui/build/niconeon-ui" } }}

perf-dummy out="perf-dummy.log" duration="60": build
    scripts/perf/run_dummy_profile.sh {{out}} {{duration}}

clean:
    cd core && cargo clean
    cd app-ui && cmake --build build --target clean
