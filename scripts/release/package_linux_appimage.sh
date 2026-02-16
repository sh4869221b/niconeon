#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <version>" >&2
  exit 1
fi

version="$1"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
out_dir="${repo_root}/dist"
ui_bin="${repo_root}/app-ui/build-release/niconeon-ui"
core_bin="${repo_root}/core/target/release/niconeon-core"
desktop_file="${repo_root}/packaging/appimage/niconeon.desktop"
icon_file="${repo_root}/packaging/appimage/niconeon.png"
license_file="${repo_root}/LICENSE"
gpl_file="${repo_root}/COPYING"
source_code_file="${repo_root}/SOURCE_CODE.md"
notices_file="${repo_root}/THIRD_PARTY_NOTICES.txt"

[[ -f "${ui_bin}" ]] || { echo "missing ui binary: ${ui_bin}" >&2; exit 1; }
[[ -f "${core_bin}" ]] || { echo "missing core binary: ${core_bin}" >&2; exit 1; }
[[ -f "${desktop_file}" ]] || { echo "missing desktop file: ${desktop_file}" >&2; exit 1; }
[[ -f "${icon_file}" ]] || { echo "missing icon file: ${icon_file}" >&2; exit 1; }
[[ -f "${license_file}" ]] || { echo "missing license file: ${license_file}" >&2; exit 1; }
[[ -f "${gpl_file}" ]] || { echo "missing gpl file: ${gpl_file}" >&2; exit 1; }
[[ -f "${source_code_file}" ]] || { echo "missing source code file: ${source_code_file}" >&2; exit 1; }
[[ -f "${notices_file}" ]] || { echo "missing notices file: ${notices_file}" >&2; exit 1; }

APPIMAGETOOL_URL="${APPIMAGETOOL_URL:-https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage}"
APPIMAGETOOL_SHA256="${APPIMAGETOOL_SHA256:-b90f4a8b18967545fda78a445b27680a1642f1ef9488ced28b65398f2be7add2}"
LINUXDEPLOY_URL="${LINUXDEPLOY_URL:-https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage}"
LINUXDEPLOY_SHA256="${LINUXDEPLOY_SHA256:-}"
LINUXDEPLOY_QT_URL="${LINUXDEPLOY_QT_URL:-https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage}"
LINUXDEPLOY_QT_SHA256="${LINUXDEPLOY_QT_SHA256:-}"

mkdir -p "${out_dir}"
tools_dir="${out_dir}/tools"
mkdir -p "${tools_dir}"

appimagetool_img="${tools_dir}/appimagetool.AppImage"
linuxdeploy_img="${tools_dir}/linuxdeploy.AppImage"
linuxdeploy_qt_img="${tools_dir}/linuxdeploy-plugin-qt.AppImage"

curl -L --retry 3 --fail -o "${appimagetool_img}" "${APPIMAGETOOL_URL}"
echo "${APPIMAGETOOL_SHA256}  ${appimagetool_img}" | sha256sum -c -
chmod +x "${appimagetool_img}"

curl -L --retry 3 --fail -o "${linuxdeploy_img}" "${LINUXDEPLOY_URL}"
if [[ -n "${LINUXDEPLOY_SHA256}" ]]; then
  echo "${LINUXDEPLOY_SHA256}  ${linuxdeploy_img}" | sha256sum -c -
fi
chmod +x "${linuxdeploy_img}"

curl -L --retry 3 --fail -o "${linuxdeploy_qt_img}" "${LINUXDEPLOY_QT_URL}"
if [[ -n "${LINUXDEPLOY_QT_SHA256}" ]]; then
  echo "${LINUXDEPLOY_QT_SHA256}  ${linuxdeploy_qt_img}" | sha256sum -c -
fi
chmod +x "${linuxdeploy_qt_img}"

staging="$(mktemp -d "${TMPDIR:-/tmp}/niconeon-appimage-XXXXXX")"
trap 'rm -rf "${staging}"' EXIT

app_dir="${staging}/AppDir"
mkdir -p "${app_dir}/usr/bin" "${app_dir}/usr/share/applications" "${app_dir}/usr/share/icons/hicolor/256x256/apps"
mkdir -p "${app_dir}/usr/share/licenses/niconeon"

cp "${ui_bin}" "${app_dir}/usr/bin/niconeon-ui"
cp "${core_bin}" "${app_dir}/usr/bin/niconeon-core"
chmod 755 "${app_dir}/usr/bin/niconeon-ui" "${app_dir}/usr/bin/niconeon-core"

cat > "${app_dir}/usr/bin/niconeon" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export NICONEON_CORE_BIN="${here}/niconeon-core"
exec "${here}/niconeon-ui" "$@"
WRAPPER
chmod 755 "${app_dir}/usr/bin/niconeon"

cp "${desktop_file}" "${app_dir}/usr/share/applications/niconeon.desktop"
cp "${desktop_file}" "${app_dir}/niconeon.desktop"
cp "${icon_file}" "${app_dir}/usr/share/icons/hicolor/256x256/apps/niconeon.png"
cp "${icon_file}" "${app_dir}/niconeon.png"
cp "${license_file}" "${app_dir}/usr/share/licenses/niconeon/LICENSE"
cp "${gpl_file}" "${app_dir}/usr/share/licenses/niconeon/COPYING"
cp "${source_code_file}" "${app_dir}/usr/share/licenses/niconeon/SOURCE_CODE.md"
cp "${notices_file}" "${app_dir}/usr/share/licenses/niconeon/THIRD_PARTY_NOTICES.txt"
ln -s usr/bin/niconeon "${app_dir}/AppRun"

cp "${linuxdeploy_qt_img}" "${tools_dir}/linuxdeploy-plugin-qt-x86_64.AppImage"
export PATH="${tools_dir}:${PATH}"
APPIMAGE_EXTRACT_AND_RUN=1 \
  "${linuxdeploy_img}" \
  --appdir "${app_dir}" \
  -e "${app_dir}/usr/bin/niconeon-ui" \
  -e "${app_dir}/usr/bin/niconeon-core" \
  -e "${app_dir}/usr/bin/niconeon" \
  -d "${app_dir}/usr/share/applications/niconeon.desktop" \
  -i "${app_dir}/usr/share/icons/hicolor/256x256/apps/niconeon.png" \
  --plugin qt \
  --deploy-deps-only

out_appimage="${out_dir}/niconeon-${version}-linux-x86_64.AppImage"
APPIMAGE_EXTRACT_AND_RUN=1 "${appimagetool_img}" "${app_dir}" "${out_appimage}"
chmod 755 "${out_appimage}"

echo "created: ${out_appimage}"
