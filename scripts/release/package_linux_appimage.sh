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
app_bin="${build_dir}/niconeon"
desktop_file="${repo_root}/packaging/appimage/niconeon.desktop"
icon_file="${repo_root}/packaging/appimage/niconeon.png"
license_file="${repo_root}/LICENSE"
gpl_file="${repo_root}/COPYING"
source_code_file="${repo_root}/SOURCE_CODE.md"
notices_file="${repo_root}/THIRD_PARTY_NOTICES.txt"

[[ -f "${app_bin}" ]] || { echo "missing application binary: ${app_bin}" >&2; exit 1; }
[[ -f "${desktop_file}" ]] || { echo "missing desktop file: ${desktop_file}" >&2; exit 1; }
[[ -f "${icon_file}" ]] || { echo "missing icon file: ${icon_file}" >&2; exit 1; }
[[ -f "${license_file}" ]] || { echo "missing license file: ${license_file}" >&2; exit 1; }
[[ -f "${gpl_file}" ]] || { echo "missing gpl file: ${gpl_file}" >&2; exit 1; }
[[ -f "${source_code_file}" ]] || { echo "missing source code file: ${source_code_file}" >&2; exit 1; }
[[ -f "${notices_file}" ]] || { echo "missing notices file: ${notices_file}" >&2; exit 1; }

APPIMAGETOOL_URL="${APPIMAGETOOL_URL:-https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage}"
APPIMAGETOOL_SHA256="${APPIMAGETOOL_SHA256:-b90f4a8b18967545fda78a445b27680a1642f1ef9488ced28b65398f2be7add2}"
LINUXDEPLOY_URL="${LINUXDEPLOY_URL:-https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage}"
LINUXDEPLOY_SHA256="${LINUXDEPLOY_SHA256:-8aea8da0f7f7039d2a2cecb14657d752a222a5e1d3825caeef186c82f751cdd1}"
LINUXDEPLOY_QT_URL="${LINUXDEPLOY_QT_URL:-https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage}"
LINUXDEPLOY_QT_SHA256="${LINUXDEPLOY_QT_SHA256:-cfc1055b2b9dbc08412b579f20990b7b41a17b61beaa5847dc9477c96c9e9617}"

mkdir -p "${out_dir}"
tools_dir="${out_dir}/tools"
mkdir -p "${tools_dir}"

appimagetool_img="${tools_dir}/appimagetool.AppImage"
linuxdeploy_img="${tools_dir}/linuxdeploy.AppImage"
linuxdeploy_qt_img="${tools_dir}/linuxdeploy-plugin-qt.AppImage"

fetch_verified_tool() {
  local url="$1" file="$2" checksum="$3"
  if [[ -f "${file}" ]] && echo "${checksum}  ${file}" | sha256sum --check --status; then
    echo "verified cached tool: ${file}"
  else
    curl -L --retry 3 --fail -o "${file}" "${url}"
    echo "${checksum}  ${file}" | sha256sum --check -
  fi
  chmod +x "${file}"
}
fetch_verified_tool "${APPIMAGETOOL_URL}" "${appimagetool_img}" "${APPIMAGETOOL_SHA256}"
fetch_verified_tool "${LINUXDEPLOY_URL}" "${linuxdeploy_img}" "${LINUXDEPLOY_SHA256}"
fetch_verified_tool "${LINUXDEPLOY_QT_URL}" "${linuxdeploy_qt_img}" "${LINUXDEPLOY_QT_SHA256}"

staging="$(mktemp -d "${TMPDIR:-/tmp}/niconeon-appimage-XXXXXX")"
trap 'rm -rf "${staging}"' EXIT

# Verify before deployment tools rewrite ELF paths.
mpv_runtime="$(ldd "$app_bin" | awk '$1 ~ /^libmpv[.]so/ { sub(/^.* => /, ""); sub(/ \(0x[^)]*\).*$/, ""); print; exit }')"
"${repo_root}/scripts/release/verify_mpv_runtime.sh" "$mpv_runtime"
app_dir="${staging}/AppDir"
mkdir -p "${app_dir}/usr/bin" "${app_dir}/usr/share/applications" "${app_dir}/usr/share/icons/hicolor/256x256/apps"
mkdir -p "${app_dir}/usr/share/licenses/niconeon"

cp "${app_bin}" "${app_dir}/usr/bin/niconeon"
chmod 755 "${app_dir}/usr/bin/niconeon"

cp "${desktop_file}" "${app_dir}/usr/share/applications/niconeon.desktop"
cp "${desktop_file}" "${app_dir}/niconeon.desktop"
cp "${icon_file}" "${app_dir}/usr/share/icons/hicolor/256x256/apps/niconeon.png"
cp "${icon_file}" "${app_dir}/niconeon.png"
cp "${license_file}" "${app_dir}/usr/share/licenses/niconeon/LICENSE"
cp "${gpl_file}" "${app_dir}/usr/share/licenses/niconeon/COPYING"
cp "${source_code_file}" "${app_dir}/usr/share/licenses/niconeon/SOURCE_CODE.md"
cp "${notices_file}" "${app_dir}/usr/share/licenses/niconeon/THIRD_PARTY_NOTICES.txt"
"${repo_root}/scripts/release/copy_mpv_source.sh" "${app_dir}/usr/share/licenses/niconeon"
cp "${repo_root}/packaging/appimage/AppRun" "${app_dir}/AppRun"
chmod 755 "${app_dir}/AppRun"

cp "${linuxdeploy_qt_img}" "${tools_dir}/linuxdeploy-plugin-qt-x86_64.AppImage"
export PATH="${tools_dir}:${PATH}"
export QML_SOURCES_PATHS="${repo_root}/src/ui/qml"
export EXTRA_QT_PLUGINS="sqldrivers"
APPIMAGE_EXTRACT_AND_RUN=1 \
  "${linuxdeploy_img}" \
  --appdir "${app_dir}" \
  -e "${app_dir}/usr/bin/niconeon" \
  -d "${app_dir}/usr/share/applications/niconeon.desktop" \
  -i "${app_dir}/usr/share/icons/hicolor/256x256/apps/niconeon.png" \
  --plugin qt \
  --deploy-deps-only "${app_dir}/usr/bin"

# Keep the loader-aware AppRun, even when a deployment tool replaces it.
cp "${repo_root}/packaging/appimage/AppRun" "${app_dir}/AppRun"
chmod 755 "${app_dir}/AppRun"
out_appimage="${out_dir}/${base}.AppImage"
APPIMAGE_EXTRACT_AND_RUN=1 "${appimagetool_img}" "${app_dir}" "${out_appimage}"
chmod 755 "${out_appimage}"

echo "created: ${out_appimage}"
