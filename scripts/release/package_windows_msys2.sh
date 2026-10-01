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


base="${NICONEON_RELEASE_BASENAME:-niconeon-${version}-windows-x86_64}"
out_zip="${out_dir}/${base}-binaries.zip"
app_exe="${build_dir}/niconeon.exe"
license_file="${repo_root}/LICENSE"
gpl_file="${repo_root}/COPYING"
source_code_file="${repo_root}/SOURCE_CODE.md"
notices_file="${repo_root}/THIRD_PARTY_NOTICES.txt"

[[ -f "${app_exe}" ]] || { echo "missing application: ${app_exe}" >&2; exit 1; }
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

collect_ucrt_dll_deps() {
  local output_file="$1"
  shift
  local -a pending=("$@")
  local current=""
  local dep=""
  declare -A visited=()

  : > "${output_file}"

  while [[ ${#pending[@]} -gt 0 ]]; do
    current="${pending[0]}"
    pending=("${pending[@]:1}")
    [[ -f "${current}" ]] || continue

    while IFS= read -r dep; do
      [[ -n "${dep}" ]] || continue
      [[ -f "${dep}" ]] || continue

      if [[ "${dep}" != /ucrt64/bin/*.dll ]]; then
        continue
      fi

      if [[ -n "${visited["${dep}"]:-}" ]]; then
        continue
      fi

      visited["${dep}"]=1
      pending+=("${dep}")
      printf '%s\n' "${dep}" >> "${output_file}"
    done < <(
      ldd "${current}" 2>/dev/null | awk '
        /=>/ {
          if ($3 ~ /^\/ucrt64\/bin\/.*\.dll$/) print $3
        }
        /^[[:space:]]*\/ucrt64\/bin\/.*\.dll/ {
          path = $1
          sub(/^[[:space:]]+/, "", path)
          if (path ~ /^\/ucrt64\/bin\/.*\.dll$/) print path
        }
      '
    )
  done
}

copy_ucrt_dep_tree() {
  local dep_file="$1"
  local output_dir="$2"
  local dep=""

  [[ -f "${dep_file}" ]] || return 0

  while IFS= read -r dep; do
    [[ -n "${dep}" ]] || continue
    cp -n "${dep}" "${output_dir}/$(basename "${dep}")"
  done < "${dep_file}"
}

# MSYS2 Qt packages place helper tools under /ucrt64/share/qt6/bin.
# Ensure windeployqt can find qmlimportscanner from PATH.
if [[ -d "/ucrt64/share/qt6/bin" ]]; then
  export PATH="/ucrt64/share/qt6/bin:${PATH}"
fi

if ! command -v qmlimportscanner.exe >/dev/null 2>&1; then
  for candidate in \
    /ucrt64/share/qt6/bin/qmlimportscanner.exe \
    /ucrt64/share/qt6/bin/qmlimportscanner-qt6.exe \
    /ucrt64/bin/qmlimportscanner-qt6.exe; do
    if [[ -f "${candidate}" ]]; then
      tool_shim_dir="$(mktemp -d "${TMPDIR:-/tmp}/niconeon-tools-XXXXXX")"
      cp "${candidate}" "${tool_shim_dir}/qmlimportscanner.exe"
      export PATH="${tool_shim_dir}:${PATH}"
      break
    fi
  done
fi

mkdir -p "${staging}/${base}"
cp "${app_exe}" "${staging}/${base}/niconeon.exe"
cp "${license_file}" "${staging}/${base}/LICENSE"
cp "${gpl_file}" "${staging}/${base}/COPYING"
cp "${source_code_file}" "${staging}/${base}/SOURCE_CODE.md"
cp "${notices_file}" "${staging}/${base}/THIRD_PARTY_NOTICES.txt"

# Ensure Qt resolves bundled plugins/QML modules from the app directory.
cat >"${staging}/${base}/qt.conf" <<'EOF'
[Paths]
Prefix = .
Plugins = .
QmlImports = qml
Qml2Imports = qml
EOF

for dll in libmpv-2.dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
  if [[ -f "/ucrt64/bin/${dll}" ]]; then
    cp "/ucrt64/bin/${dll}" "${staging}/${base}/${dll}"
  fi
done

# SQLite is required for cache/filter persistence, including deployments with no QML SQL imports.
for plugin in /ucrt64/share/qt6/plugins/sqldrivers/qsqlite.dll /ucrt64/lib/qt6/plugins/sqldrivers/qsqlite.dll; do
  if [[ -f "${plugin}" ]]; then
    mkdir -p "${staging}/${base}/sqldrivers"
    cp "${plugin}" "${staging}/${base}/sqldrivers/"
  fi
done

# Some Qt builds depend on these DLL families and users hit runtime errors
# when they are omitted (e.g. libmd4c/libdouble-conversion/ICU).
for pattern in libmd4c.dll libdouble-conversion.dll libicu*.dll; do
  for candidate in /ucrt64/bin/${pattern}; do
    if [[ -f "${candidate}" ]]; then
      cp -n "${candidate}" "${staging}/${base}/$(basename "${candidate}")"
    fi
  done
done

"${windeployqt_bin}" \
  --release \
  --no-translations \
  --qmldir "${repo_root}/src/ui/qml" \
  "${staging}/${base}/niconeon.exe"

# Ensure transitive runtime dependencies for all packaged binaries/plugins
# are present in the top-level app directory.
dep_list_file="${staging}/ucrt-deps.txt"
dep_roots=("${staging}/${base}/niconeon.exe")
if [[ -f "/ucrt64/bin/libmpv-2.dll" ]]; then
  dep_roots+=("/ucrt64/bin/libmpv-2.dll")
fi
while IFS= read -r -d '' binary; do
  dep_roots+=("${binary}")
done < <(find "${staging}/${base}" -type f \( -iname "*.exe" -o -iname "*.dll" \) -print0)
collect_ucrt_dll_deps "${dep_list_file}" "${dep_roots[@]}"
copy_ucrt_dep_tree "${dep_list_file}" "${staging}/${base}"

echo "staging size before zip:"
du -sh "${staging}/${base}" || true

(
  cd "${staging}"
  zip -r -1 "${out_zip}" "${base}" >/dev/null
)

echo "created: ${out_zip}"
