#!/usr/bin/env bash
# Build a shared, security-patched Qt SDK on the same Debian 13 as its consumers.
set -euo pipefail
[[ $# -eq 1 ]] || { echo "usage: $0 <absolute-install-prefix>" >&2; exit 2; }
prefix="$1"
[[ "$prefix" == /* ]] || { echo "prefix must be absolute" >&2; exit 2; }
[[ "$(uname -s)" == Linux ]] || { echo "this source build is for Linux" >&2; exit 2; }
script_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
manifest="$script_dir/qt-sources.sha256"
work="${NICONEON_QT_WORK_DIR:-${script_dir}/../../build/_deps/qt}"
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "invalid build parallelism" >&2; exit 2; }
mkdir -p "$work" "$prefix"
work="$(cd "$work" && pwd)"
prefix="$(cd "$prefix" && pwd)"
[[ ! -e "$prefix/lib/libQt6Core.so" ]] || {
  echo "use a clean Qt prefix; refusing to mix an existing SDK with this build" >&2; exit 1;
}
for command in cmake ninja curl tar patch sha256sum python3; do
  command -v "$command" >/dev/null || { echo "missing build tool: $command" >&2; exit 1; }
done
base_url=https://download.qt.io/official_releases/qt/6.12
while read -r checksum file; do
  [[ "$checksum" =~ ^[0-9a-f]{64}$ && "$file" =~ ^[A-Za-z0-9._-]+$ ]] || exit 1
  case "$file" in
    *.tar.xz) url="$base_url/6.12.0/submodules/$file" ;;
    *.diff) url="$base_url/$file" ;;
    *) echo "unsupported source manifest entry: $file" >&2; exit 1 ;;
  esac
  if [[ ! -f "$work/$file" ]]; then
    curl --fail --location --retry 3 --connect-timeout 30 --max-time 600 "$url" -o "$work/$file.download"
    printf '%s  %s\n' "$checksum" "$work/$file.download" | sha256sum --check --status
    mv "$work/$file.download" "$work/$file"
  fi
  printf '%s  %s\n' "$checksum" "$work/$file" | sha256sum --check --status
done < "$manifest"

# Extract fresh sources; never reuse an already patched or externally edited tree.
run_dir="$(mktemp -d "$work/build-6.12.0.XXXXXX")"
modules=(qtbase qtshadertools qtsvg qtdeclarative)
for module in "${modules[@]}"; do
  tar -xJf "$work/$module-everywhere-src-6.12.0.tar.xz" -C "$run_dir"
done
# The upstream numbering is NOT application order: 0001's tests depend on 0002.
for file in CVE-2026-105924-qtbase-6.12.diff \
  CVE-2026-116337-qtbase-6.12-0002.diff CVE-2026-116337-qtbase-6.12-0001.diff; do
  patch --batch --forward --fuzz=0 -d "$run_dir/qtbase-everywhere-src-6.12.0" -p1 < "$work/$file"
done

common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix"
  -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=ON -DQT_BUILD_EXAMPLES=OFF
  -DQT_BUILD_TESTS=OFF -DQT_BUILD_BENCHMARKS=OFF -DQT_BUILD_DOCS=OFF
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON)
# Disable upstream's example/test executables, not the Test / QuickTest SDK or
# any application tests. Require the actual GL/X11/EGL/TLS/SQL features we use.
cmake -S "$run_dir/qtbase-everywhere-src-6.12.0" -B "$run_dir/build-qtbase" "${common[@]}" \
  -DFEATURE_gui=ON -DFEATURE_widgets=ON -DFEATURE_network=ON \
  -DFEATURE_sql=ON -DFEATURE_testlib=ON -DFEATURE_concurrent=ON \
  -DINPUT_opengl=desktop -DFEATURE_opengl=ON -DFEATURE_egl=ON \
  -DFEATURE_xcb=ON -DFEATURE_xcb_xlib=ON -DFEATURE_xcb_glx_plugin=ON \
  -DFEATURE_xcb_egl_plugin=ON -DFEATURE_fontconfig=ON -DFEATURE_dbus=ON \
  -DFEATURE_wayland=ON -DFEATURE_wayland_client=ON -DFEATURE_wayland_egl=ON \
  -DFEATURE_openssl_linked=ON -DFEATURE_sql_sqlite=ON -DFEATURE_system_sqlite=ON
cmake --build "$run_dir/build-qtbase" --parallel "$jobs"
cmake --install "$run_dir/build-qtbase"
# ShaderTools provides qsb, required to build Qt Quick. SVG precedes Declarative
# so its optional SVG / VectorImage support is not silently omitted.
for module in qtshadertools qtsvg qtdeclarative; do
  extra=()
  if [[ "$module" == qtdeclarative ]]; then
    extra=(-DFEATURE_qml_network=ON -DFEATURE_qml_ssl=ON
      -DFEATURE_qml_worker_script=ON -DFEATURE_qml_animation=ON
      -DFEATURE_quick_dialogs=ON -DFEATURE_quick_shadereffect=ON)
  fi
  "$prefix/bin/qt-cmake" -S "$run_dir/$module-everywhere-src-6.12.0" \
    -B "$run_dir/build-$module" "${common[@]}" "${extra[@]}"
  cmake --build "$run_dir/build-$module" --parallel "$jobs"
  cmake --install "$run_dir/build-$module"
done

# Fail before publishing an incomplete SDK, including private renderer headers.
for file in bin/qmake bin/qt-cmake libexec/qmlimportscanner \
  lib/cmake/Qt6GuiPrivate/Qt6GuiPrivateConfig.cmake \
  include/QtGui/6.12.0/QtGui/private/qrhi_p.h \
  lib/cmake/Qt6QuickControls2/Qt6QuickControls2Config.cmake \
  lib/cmake/Qt6QuickTest/Qt6QuickTestConfig.cmake \
  plugins/platforms/libqxcb.so plugins/platforms/libqwayland.so \
  plugins/sqldrivers/libqsqlite.so plugins/tls/libqopensslbackend.so \
  qml/QtQuick/Controls/qmldir qml/QtQuick/Dialogs/qmldir \
  qml/QtQuick/Layouts/qmldir qml/QtQml/WorkerScript/qmldir \
  qml/QtCore/qmldir qml/QtTest/qmldir; do
  [[ -f "$prefix/$file" ]] || { echo "missing required Qt SDK component: $file" >&2; exit 1; }
done
[[ "$("$prefix/bin/qmake" -query QT_VERSION)" == 6.12.0 ]] || exit 1

source_bundle="$prefix/share/niconeon/qt-source"
mkdir -p "$source_bundle/notices" "$source_bundle/configuration"
cp "$manifest" "$script_dir/build_qt.sh" "$source_bundle/"
while read -r checksum file; do cp "$work/$file" "$source_bundle/"; done < "$manifest"
for module in "${modules[@]}"; do
  mkdir -p "$source_bundle/notices/$module"
  cp -R "$run_dir/$module-everywhere-src-6.12.0/LICENSES" "$source_bundle/notices/$module/"
  # Full archives retain all embedded and third-party notices, not just SPDX texts.
  cp "$run_dir/build-$module/CMakeCache.txt" "$source_bundle/configuration/$module-CMakeCache.txt"
  if [[ -f "$run_dir/build-$module/config.summary" ]]; then
    cp "$run_dir/build-$module/config.summary" "$source_bundle/configuration/$module-summary.txt"
  fi
done
(
  cd "$prefix"
  find lib plugins qml -type f -name '*.so*' -print0 | LC_ALL=C sort -z | xargs -0 sha256sum
) > "$source_bundle/runtime.sha256"
[[ -s "$source_bundle/runtime.sha256" ]] || exit 1
cat > "$source_bundle/README.txt" <<EOF
Qt 6.12.0 with the official Qt security fixes available on 2026-11-12.
Modules, in build order: qtbase, qtshadertools, qtsvg, qtdeclarative.
Source archives: $base_url/6.12.0/submodules/<filename>
Patches: $base_url/<filename>
qt-sources.sha256 pins every original archive and patch. Tarball digests were
also checked against download.qt.io's published SHA-256 metadata.
Apply CVE-2026-105924, then CVE-2026-116337-0002, then -0001, without fuzz.
The patches include their upstream regression tests. All changes are retained.
Qt 6.12.1 was not published in the official stable source index on this date.
Security advisories:
https://www.qt.io/blog/security-advisory-cve-2026-105924
https://www.qt.io/blog/security-issue-cve-2026-116337
Other post-6.12.0 advisories affect modules not built here (Qt5Compat, Qt3D,
QtHttpServer, QtWebSockets, QtMultimedia). Previously fixed QtBase/QtQuick/SVG
issues are covered by 6.12.0. This is a dated review, not a future safety claim.

Reproduce: install the Debian 13 development dependencies documented in the
repository, put CMake and Ninja on PATH, then run bash build_qt.sh <prefix>.
Set NICONEON_QT_WORK_DIR to this source directory to use the retained archives.
Default build parallelism is 2; CMAKE_BUILD_PARALLEL_LEVEL overrides it.
Build and consume the SDK at the same absolute prefix: $prefix
The shared SDK contains GuiPrivate and the Qt Test/QuickTest libraries.
Only Qt's own test/example/benchmark/documentation executables are not built;
Niconeon's unit, QML, sanitizer, and strict OpenGL tests remain mandatory.
QtBase includes the Wayland client in 6.12; QtWayland's compositor is not needed.
Wayland support is compiled in, not a claim of Niconeon runtime qualification.

LICENSES directories retain each module's license texts; exact source archives
retain complete copyright, third-party notices and bundled-library sources.
Use the applicable GPL/LGPL terms for each shipped component. Shared linkage
and replaceable libraries are preserved. System libraries remain separately
licensed dependencies; retain their distributor notices and corresponding
sources when bundling them, including the system SQLite and OpenSSL runtimes.
configuration/ records the actual build options. runtime.sha256 identifies the
SDK's libraries and plugins before deployment tools rewrite their ELF paths.
EOF
printf 'Installed patched Qt 6.12.0 and corresponding source into %s\n' "$prefix"
