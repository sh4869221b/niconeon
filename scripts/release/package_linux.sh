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
license_file="${repo_root}/LICENSE"
gpl_file="${repo_root}/COPYING"
source_code_file="${repo_root}/SOURCE_CODE.md"
notices_file="${repo_root}/THIRD_PARTY_NOTICES.txt"

[[ -f "${ui_bin}" ]] || { echo "missing ui binary: ${ui_bin}" >&2; exit 1; }
[[ -f "${core_bin}" ]] || { echo "missing core binary: ${core_bin}" >&2; exit 1; }
[[ -f "${license_file}" ]] || { echo "missing license file: ${license_file}" >&2; exit 1; }
[[ -f "${gpl_file}" ]] || { echo "missing gpl file: ${gpl_file}" >&2; exit 1; }
[[ -f "${source_code_file}" ]] || { echo "missing source code file: ${source_code_file}" >&2; exit 1; }
[[ -f "${notices_file}" ]] || { echo "missing notices file: ${notices_file}" >&2; exit 1; }

mkdir -p "${out_dir}"
staging="$(mktemp -d "${TMPDIR:-/tmp}/niconeon-linux-XXXXXX")"
trap 'rm -rf "${staging}"' EXIT

mkdir -p "${staging}/${base}"
cp "${ui_bin}" "${staging}/${base}/niconeon-ui"
cp "${core_bin}" "${staging}/${base}/niconeon-core"
cp "${license_file}" "${staging}/${base}/LICENSE"
cp "${gpl_file}" "${staging}/${base}/COPYING"
cp "${source_code_file}" "${staging}/${base}/SOURCE_CODE.md"
cp "${notices_file}" "${staging}/${base}/THIRD_PARTY_NOTICES.txt"
chmod 755 "${staging}/${base}/niconeon-ui" "${staging}/${base}/niconeon-core"

(
  cd "${staging}"
  zip -r "${out_zip}" "${base}" >/dev/null
)

echo "created: ${out_zip}"
