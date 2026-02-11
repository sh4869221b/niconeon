#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <version>" >&2
  exit 1
fi

version="$1"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
out_dir="${repo_root}/dist"
base="niconeon-${version}-windows-x86_64"
out_zip="${out_dir}/${base}-binaries.zip"
ui_exe="${repo_root}/app-ui/build-release/niconeon-ui.exe"
core_exe="${repo_root}/core/target/release/niconeon-core.exe"

[[ -f "${ui_exe}" ]] || { echo "missing ui exe: ${ui_exe}" >&2; exit 1; }
[[ -f "${core_exe}" ]] || { echo "missing core exe: ${core_exe}" >&2; exit 1; }

if command -v windeployqt6.exe >/dev/null 2>&1; then
  windeployqt_bin="$(command -v windeployqt6.exe)"
elif command -v windeployqt.exe >/dev/null 2>&1; then
  windeployqt_bin="$(command -v windeployqt.exe)"
else
  echo "windeployqt not found in PATH" >&2
  exit 1
fi

mkdir -p "${out_dir}"
staging="$(mktemp -d "${TMPDIR:-/tmp}/niconeon-win-XXXXXX")"
trap 'rm -rf "${staging}"' EXIT

mkdir -p "${staging}/${base}"
cp "${ui_exe}" "${staging}/${base}/niconeon-ui.exe"
cp "${core_exe}" "${staging}/${base}/niconeon-core.exe"

for dll in libmpv-2.dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
  if [[ -f "/mingw64/bin/${dll}" ]]; then
    cp "/mingw64/bin/${dll}" "${staging}/${base}/${dll}"
  fi
done

"${windeployqt_bin}" --release --qmldir "${repo_root}/app-ui/qml" "${staging}/${base}/niconeon-ui.exe"

(
  cd "${staging}"
  zip -r "${out_zip}" "${base}" >/dev/null
)

echo "created: ${out_zip}"
