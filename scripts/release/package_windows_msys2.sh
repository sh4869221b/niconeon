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
license_file="${repo_root}/LICENSE"
gpl_file="${repo_root}/COPYING"
source_code_file="${repo_root}/SOURCE_CODE.md"
notices_file="${repo_root}/THIRD_PARTY_NOTICES.txt"

[[ -f "${ui_exe}" ]] || { echo "missing ui exe: ${ui_exe}" >&2; exit 1; }
[[ -f "${core_exe}" ]] || { echo "missing core exe: ${core_exe}" >&2; exit 1; }
[[ -f "${license_file}" ]] || { echo "missing license file: ${license_file}" >&2; exit 1; }
[[ -f "${gpl_file}" ]] || { echo "missing gpl file: ${gpl_file}" >&2; exit 1; }
[[ -f "${source_code_file}" ]] || { echo "missing source code file: ${source_code_file}" >&2; exit 1; }
[[ -f "${notices_file}" ]] || { echo "missing notices file: ${notices_file}" >&2; exit 1; }

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
tool_shim_dir=""
trap 'rm -rf "${staging}" "${tool_shim_dir}"' EXIT

collect_mingw_dll_deps() {
  local root="$1"
  local -a pending=("${root}")
  local current=""
  local dep=""
  declare -A visited=()

  while [[ ${#pending[@]} -gt 0 ]]; do
    current="${pending[0]}"
    pending=("${pending[@]:1}")

    while IFS= read -r dep; do
      [[ -n "${dep}" ]] || continue
      [[ -f "${dep}" ]] || continue

      if [[ "${dep}" != /mingw64/bin/*.dll ]]; then
        continue
      fi

      if [[ -n "${visited["${dep}"]:-}" ]]; then
        continue
      fi

      visited["${dep}"]=1
      pending+=("${dep}")
      printf '%s\n' "${dep}"
    done < <(
      ldd "${current}" 2>/dev/null | awk '
        /=>/ {
          if ($3 ~ /^\/mingw64\/bin\/.*\.dll$/) print $3
        }
        /^[[:space:]]*\/mingw64\/bin\/.*\.dll/ {
          path = $1
          sub(/^[[:space:]]+/, "", path)
          if (path ~ /^\/mingw64\/bin\/.*\.dll$/) print path
        }
      '
    )
  done
}

copy_mingw_dep_tree() {
  local root="$1"
  local output_dir="$2"
  local dep=""

  [[ -f "${root}" ]] || return 0

  while IFS= read -r dep; do
    [[ -n "${dep}" ]] || continue
    cp -n "${dep}" "${output_dir}/$(basename "${dep}")"
  done < <(collect_mingw_dll_deps "${root}")
}

# MSYS2 Qt packages place helper tools under /mingw64/share/qt6/bin.
# Ensure windeployqt can find qmlimportscanner from PATH.
if [[ -d "/mingw64/share/qt6/bin" ]]; then
  export PATH="/mingw64/share/qt6/bin:${PATH}"
fi

if [[ ! -f "/mingw64/bin/qmlimportscanner.exe" ]]; then
  for candidate in \
    /mingw64/share/qt6/bin/qmlimportscanner.exe \
    /mingw64/share/qt6/bin/qmlimportscanner-qt6.exe \
    /mingw64/bin/qmlimportscanner-qt6.exe; do
    if [[ -f "${candidate}" ]]; then
      ln -sf "${candidate}" /mingw64/bin/qmlimportscanner.exe 2>/dev/null || true
      break
    fi
  done
fi

if ! command -v qmlimportscanner.exe >/dev/null 2>&1; then
  for candidate in \
    /mingw64/share/qt6/bin/qmlimportscanner.exe \
    /mingw64/share/qt6/bin/qmlimportscanner-qt6.exe \
    /mingw64/bin/qmlimportscanner-qt6.exe; do
    if [[ -f "${candidate}" ]]; then
      tool_shim_dir="$(mktemp -d "${TMPDIR:-/tmp}/niconeon-tools-XXXXXX")"
      cp "${candidate}" "${tool_shim_dir}/qmlimportscanner.exe"
      export PATH="${tool_shim_dir}:${PATH}"
      break
    fi
  done
fi

mkdir -p "${staging}/${base}"
cp "${ui_exe}" "${staging}/${base}/niconeon-ui.exe"
cp "${core_exe}" "${staging}/${base}/niconeon-core.exe"
cp "${license_file}" "${staging}/${base}/LICENSE"
cp "${gpl_file}" "${staging}/${base}/COPYING"
cp "${source_code_file}" "${staging}/${base}/SOURCE_CODE.md"
cp "${notices_file}" "${staging}/${base}/THIRD_PARTY_NOTICES.txt"

for dll in libmpv-2.dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
  if [[ -f "/mingw64/bin/${dll}" ]]; then
    cp "/mingw64/bin/${dll}" "${staging}/${base}/${dll}"
  fi
done

# Some Qt builds depend on these DLL families and users hit runtime errors
# when they are omitted (e.g. libmd4c/libdouble-conversion/ICU).
for pattern in libmd4c.dll libdouble-conversion.dll libicu*.dll; do
  for candidate in /mingw64/bin/${pattern}; do
    if [[ -f "${candidate}" ]]; then
      cp -n "${candidate}" "${staging}/${base}/$(basename "${candidate}")"
    fi
  done
done

copy_mingw_dep_tree "${ui_exe}" "${staging}/${base}"
copy_mingw_dep_tree "/mingw64/bin/libmpv-2.dll" "${staging}/${base}"

"${windeployqt_bin}" \
  --release \
  --ignore-library-errors \
  --no-translations \
  --qmldir "${repo_root}/app-ui/qml" \
  "${staging}/${base}/niconeon-ui.exe"

# Ensure transitive runtime dependencies for all packaged binaries/plugins
# are present in the top-level app directory.
while IFS= read -r -d '' binary; do
  copy_mingw_dep_tree "${binary}" "${staging}/${base}"
done < <(find "${staging}/${base}" -type f \( -iname "*.exe" -o -iname "*.dll" \) -print0)

(
  cd "${staging}"
  zip -r "${out_zip}" "${base}" >/dev/null
)

echo "created: ${out_zip}"
