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

build: licenses core-build ui-configure ui-build

run: build
    {{ if os() == "windows" { "set NICONEON_CORE_BIN=core\\target\\debug\\niconeon-core.exe && app-ui\\build\\niconeon-ui.exe" } else { "NICONEON_CORE_BIN=core/target/debug/niconeon-core app-ui/build/niconeon-ui" } }}

perf-dummy out="perf-dummy.log" duration="60": build
    scripts/perf/run_dummy_profile.sh {{out}} {{duration}}

clean:
    cd core && cargo clean
    cd app-ui && cmake --build build --target clean
