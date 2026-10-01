#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <version>" >&2
  exit 1
fi

version="$1"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
out_dir="${repo_root}/dist"
build_dir="${NICONEON_BUILD_DIR:-build/release}"

if [[ "${build_dir}" != /* ]]; then
  build_dir="${repo_root}/${build_dir}"
fi


base="${NICONEON_RELEASE_BASENAME:-niconeon-${version}-linux-x86_64}"
out_zip="${out_dir}/${base}-binaries.zip"
app_bin="${build_dir}/niconeon"
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

require_file "application binary" "${app_bin}"
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
cp "${app_bin}" "${staging}/${base}/niconeon"
cp "${license_file}" "${staging}/${base}/LICENSE"
cp "${gpl_file}" "${staging}/${base}/COPYING"
cp "${source_code_file}" "${staging}/${base}/SOURCE_CODE.md"
cp "${notices_file}" "${staging}/${base}/THIRD_PARTY_NOTICES.txt"
chmod 755 "${staging}/${base}/niconeon"

(
  cd "${staging}"
  zip -r "${out_zip}" "${base}" >/dev/null
)

echo "created: ${out_zip}"
