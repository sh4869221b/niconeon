#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <version>" >&2
  exit 1
fi

version="$1"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
out_dir="${repo_root}/dist"
ui_build_dir="${NICONEON_UI_BUILD_DIR:-app-ui/build-release}"
core_build_dir="${NICONEON_CORE_BUILD_DIR:-core/target/release}"

if [[ "${ui_build_dir}" != /* ]]; then
  ui_build_dir="${repo_root}/${ui_build_dir}"
fi

if [[ "${core_build_dir}" != /* ]]; then
  core_build_dir="${repo_root}/${core_build_dir}"
fi

base="${NICONEON_RELEASE_BASENAME:-niconeon-${version}-linux-x86_64}"
out_zip="${out_dir}/${base}-binaries.zip"
ui_bin="${ui_build_dir}/niconeon-ui"
core_bin="${core_build_dir}/niconeon-core"
license_file="${repo_root}/LICENSE"
gpl_file="${repo_root}/COPYING"
source_code_file="${repo_root}/SOURCE_CODE.md"
notices_file="${repo_root}/THIRD_PARTY_NOTICES.txt"

missing_input=0
require_file() {
  local label="$1"
  local path="$2"

  if [[ ! -f "${path}" ]]; then
    echo "missing ${label}: ${path}" >&2
    missing_input=1
  fi
}

require_file "ui binary" "${ui_bin}"
require_file "core binary" "${core_bin}"
require_file "license file" "${license_file}"
require_file "gpl file" "${gpl_file}"
require_file "source code file" "${source_code_file}"
require_file "notices file" "${notices_file}"
if [[ "${missing_input}" -ne 0 ]]; then
  exit 1
fi

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
