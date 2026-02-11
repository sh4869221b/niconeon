#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <version>" >&2
  exit 1
fi

version="$1"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
out_dir="${repo_root}/dist"
base="niconeon-${version}-linux-x86_64"
out_zip="${out_dir}/${base}-binaries.zip"
ui_bin="${repo_root}/app-ui/build-release/niconeon-ui"
core_bin="${repo_root}/core/target/release/niconeon-core"

[[ -f "${ui_bin}" ]] || { echo "missing ui binary: ${ui_bin}" >&2; exit 1; }
[[ -f "${core_bin}" ]] || { echo "missing core binary: ${core_bin}" >&2; exit 1; }

mkdir -p "${out_dir}"
staging="$(mktemp -d "${TMPDIR:-/tmp}/niconeon-linux-XXXXXX")"
trap 'rm -rf "${staging}"' EXIT

mkdir -p "${staging}/${base}"
cp "${ui_bin}" "${staging}/${base}/niconeon-ui"
cp "${core_bin}" "${staging}/${base}/niconeon-core"
chmod 755 "${staging}/${base}/niconeon-ui" "${staging}/${base}/niconeon-core"

(
  cd "${staging}"
  zip -r "${out_zip}" "${base}" >/dev/null
)

echo "created: ${out_zip}"
