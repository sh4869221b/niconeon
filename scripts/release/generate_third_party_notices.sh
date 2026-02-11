#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
output_file="${1:-${repo_root}/THIRD_PARTY_NOTICES.txt}"
manual_file="${repo_root}/packaging/licenses/manual_components.toml"
manifest_path="${repo_root}/core/Cargo.toml"

if ! command -v cargo-license >/dev/null 2>&1; then
  echo "cargo-license is required. Install with: cargo install --locked cargo-license" >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 is required." >&2
  exit 1
fi

if [[ ! -f "${manual_file}" ]]; then
  echo "missing manual components file: ${manual_file}" >&2
  exit 1
fi

if [[ ! -f "${manifest_path}" ]]; then
  echo "missing cargo manifest: ${manifest_path}" >&2
  exit 1
fi

# Use a workspace-local cache location by default to avoid permission issues.
if [[ -z "${CARGO_HOME:-}" ]]; then
  export CARGO_HOME="${TMPDIR:-/tmp}/niconeon-cargo-home"
fi

mkdir -p "${CARGO_HOME}"

tmp_json="$(mktemp "${TMPDIR:-/tmp}/niconeon-license-json-XXXXXX")"
tmp_out="$(mktemp "${TMPDIR:-/tmp}/niconeon-license-out-XXXXXX")"
trap 'rm -f "${tmp_json}" "${tmp_out}"' EXIT

cargo license \
  --manifest-path "${manifest_path}" \
  --json \
  --avoid-build-deps \
  --avoid-dev-deps \
  > "${tmp_json}"

python3 - "${tmp_json}" "${manual_file}" "${tmp_out}" <<'PY'
import json
import pathlib
import tomllib
import sys

cargo_json_path = pathlib.Path(sys.argv[1])
manual_path = pathlib.Path(sys.argv[2])
out_path = pathlib.Path(sys.argv[3])

crates = json.loads(cargo_json_path.read_text(encoding="utf-8"))
manual_components = tomllib.loads(manual_path.read_text(encoding="utf-8")).get("component", [])

crate_rows = {}
for crate in crates:
    name = (crate.get("name") or "").strip()
    if not name:
        continue
    version = (crate.get("version") or "").strip()
    key = (name.lower(), version)
    crate_rows[key] = {
        "name": name,
        "version": version,
        "license": (crate.get("license") or crate.get("license_file") or "UNKNOWN").strip() or "UNKNOWN",
        "repository": (crate.get("repository") or "").strip(),
        "description": " ".join((crate.get("description") or "").split()),
    }

sorted_crates = [crate_rows[key] for key in sorted(crate_rows)]
sorted_manual = sorted(
    manual_components,
    key=lambda component: (str(component.get("name", "")).lower(), str(component.get("license", ""))),
)

lines = []
lines.append("Niconeon Third-Party Notices")
lines.append("============================")
lines.append("")
lines.append("This document lists third-party software notices for Niconeon distributions.")
lines.append("")
lines.append("Project")
lines.append("-------")
lines.append("Niconeon")
lines.append("Source code license: MIT")
lines.append("Source code license file: LICENSE")
lines.append("Binary distribution terms: GPL-3.0-or-later")
lines.append("Binary distribution license file: COPYING")
lines.append("Source code availability: SOURCE_CODE.md")
lines.append("")
lines.append("Manual Runtime Components")
lines.append("-------------------------")

if not sorted_manual:
    lines.append("- (none)")
else:
    for component in sorted_manual:
        name = str(component.get("name", "")).strip()
        license_expr = str(component.get("license", "UNKNOWN")).strip() or "UNKNOWN"
        source = str(component.get("source", "")).strip()
        notes = str(component.get("notes", "")).strip()

        if not name:
            continue

        lines.append(f"- {name}")
        lines.append(f"  License: {license_expr}")
        if source:
            lines.append(f"  Source: {source}")
        if notes:
            lines.append(f"  Notes: {notes}")
        lines.append("")

lines.append("Rust Dependencies (core/Cargo.toml)")
lines.append("------------------------------------")

if not sorted_crates:
    lines.append("- (none)")
else:
    for crate in sorted_crates:
        display_name = crate["name"]
        if crate["version"]:
            display_name = f"{display_name} {crate['version']}"

        lines.append(f"- {display_name}")
        lines.append(f"  License: {crate['license']}")
        if crate["repository"]:
            lines.append(f"  Repository: {crate['repository']}")
        if crate["description"]:
            lines.append(f"  Description: {crate['description']}")
        lines.append("")

out_path.write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")
PY

mkdir -p "$(dirname "${output_file}")"
cp "${tmp_out}" "${output_file}"
echo "created: ${output_file}"
