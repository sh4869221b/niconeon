#!/usr/bin/env bash
# Build Linux subtitle/color libraries without raising the Debian 13 baseline.
set -euo pipefail
[[ $# -eq 1 && "$1" == /* ]] || { echo "usage: $0 <absolute-install-prefix>" >&2; exit 2; }
[[ "$(uname -s)" == Linux ]] || { echo "this source build is for Linux" >&2; exit 2; }
prefix="$1"
script_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
manifest="$script_dir/media-support-sources.sha256"
work="${NICONEON_MEDIA_SUPPORT_WORK_DIR:-${script_dir}/../../build/_deps/media-support}"
meson="${NICONEON_MESON:-meson}"
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "parallel job count must be positive" >&2; exit 2; }
for tool in curl tar sha256sum pkg-config make readelf python3; do
  command -v "$tool" >/dev/null || { echo "required build tool not found: $tool" >&2; exit 1; }
done
command -v "$meson" >/dev/null || { echo "Meson is required; use the verified build-tools SDK" >&2; exit 1; }
# These are mandatory, not opportunistic fallbacks that remove subtitle features.
pkg-config --exists 'freetype2 >= 9.17.3' 'fontconfig >= 2.10.92' \
  'fribidi >= 0.19.1' 'harfbuzz >= 1.2.3' 'libunibreak >= 1.1' || {
  echo "install libfreetype-dev libfontconfig1-dev libfribidi-dev libharfbuzz-dev libunibreak-dev" >&2
  exit 1
}
case "$(uname -m)" in
  x86_64|i?86) command -v nasm >/dev/null || { echo "nasm is required for libass assembly" >&2; exit 1; } ;;
esac
mkdir -p "$work" "$prefix"
work="$(cd "$work" && pwd)"
prefix="$(cd "$prefix" && pwd)"
fetch() {
  local name="$1" url="$2" checksum archive
  archive="$work/$name"
  checksum="$(awk -v name="$name" '$2 == name { print $1 }' "$manifest")"
  [[ "$checksum" =~ ^[0-9a-f]{64}$ ]] || { echo "missing source pin: $name" >&2; exit 1; }
  if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 2 --connect-timeout 30 --max-time 180 "$url" -o "$archive.download"
    printf '%s  %s\n' "$checksum" "$archive.download" | sha256sum --check --status
    mv "$archive.download" "$archive"
  fi
  printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status
}
fetch libass-0.17.5.tar.xz https://github.com/libass/libass/releases/download/0.17.5/libass-0.17.5.tar.xz
fetch lcms2-2.20.tar.gz https://github.com/mm2/Little-CMS/releases/download/lcms2.20/lcms2-2.20.tar.gz

# Always use fresh verified source, never a previously edited extraction/build.
run_dir="$(mktemp -d "$work/build-0.17.5-2.20.XXXXXX")"
tar -xJf "$work/libass-0.17.5.tar.xz" -C "$run_dir"
tar -xzf "$work/lcms2-2.20.tar.gz" -C "$run_dir"
mkdir -p "$run_dir/build-libass"
ass_options=("--prefix=$prefix" "--libdir=$prefix/lib" --enable-shared --disable-static
  --enable-fontconfig --enable-libunibreak --enable-asm
  --disable-test --disable-compare --disable-profile --disable-fuzz)
# libass upstream warns that its Meson shared build lacks proper symbol
# visibility. The release's generated Autotools files apply libass.sym.
# No autoreconf or source regeneration is needed.
(
  cd "$run_dir/build-libass"
  "$run_dir/libass-0.17.5/configure" "${ass_options[@]}"
  make -j "$jobs"
  make -j "$jobs" check
  make install
)
lcms_options=(--prefix "$prefix" --libdir lib --buildtype release --wrap-mode nodownload
  -Ddefault_library=shared -Dversionedlibs=true -Dtests=enabled
  -Dutils=false -Dfastfloat=false -Dthreaded=false)
"$meson" setup "$run_dir/build-lcms2" "$run_dir/lcms2-2.20" "${lcms_options[@]}"
"$meson" compile -C "$run_dir/build-lcms2" -j "$jobs"
"$meson" test -C "$run_dir/build-lcms2" --print-errorlogs
"$meson" install -C "$run_dir/build-lcms2"

# Refuse an ABI change rather than silently packaging an incompatible library.
for soname in libass.so.9 liblcms2.so.2; do
  runtime="$prefix/lib/$soname"
  [[ -f "$runtime" ]] || { echo "missing installed runtime: $runtime" >&2; exit 1; }
  actual="$(LC_ALL=C readelf -d "$runtime" | sed -n 's/.*(SONAME).*\[\([^]]*\)\].*/\1/p')"
  [[ "$actual" == "$soname" ]] || { echo "unexpected shared library ABI: $actual" >&2; exit 1; }
done
[[ "$(PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" pkg-config --modversion libass)" == 0.17.5 ]]
[[ "$(PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" pkg-config --modversion lcms2)" == 2.20 ]]

source_bundle="$prefix/share/niconeon/media-support-source"
mkdir -p "$source_bundle/configuration/libass" "$source_bundle/configuration/lcms2" \
  "$source_bundle/notices/libass" "$source_bundle/notices/lcms2"
cp "$work/libass-0.17.5.tar.xz" "$work/lcms2-2.20.tar.gz" "$manifest" \
  "$script_dir/build_media_support.sh" "$source_bundle/"
cp "$run_dir/libass-0.17.5/COPYING" "$source_bundle/notices/libass/"
cp "$run_dir/lcms2-2.20/LICENSE" "$source_bundle/notices/lcms2/"
cp "$run_dir/build-libass/"{config.log,config.status,config.h,Makefile} "$source_bundle/configuration/libass/"
cp "$run_dir/build-lcms2/meson-info/"intro-{buildoptions,dependencies}.json \
  "$run_dir/build-lcms2/compile_commands.json" "$source_bundle/configuration/lcms2/"
cp "$run_dir/build-lcms2/meson-logs/"{meson-log.txt,testlog.txt} "$source_bundle/configuration/lcms2/"
[[ ! -f "$run_dir/build-libass/test-suite.log" ]] || \
  cp "$run_dir/build-libass/test-suite.log" "$source_bundle/configuration/libass/"
{
  printf 'libass configure:'; printf ' %q' "${ass_options[@]}"; printf '\n'
  printf 'LittleCMS meson setup:'; printf ' %q' "${lcms_options[@]}"; printf '\n'
  for variable in CC CFLAGS CPPFLAGS LDFLAGS PKG_CONFIG_PATH PKG_CONFIG_LIBDIR; do
    printf '%s=%q\n' "$variable" "${!variable-}"
  done
  printf 'Meson: '; "$meson" --version
  printf 'Make: '; make --version
  printf '\nSystem baseline:\n'; cat /etc/os-release
  printf '\nSubtitle dependency versions:\n'
  for module in freetype2 fontconfig fribidi harfbuzz libunibreak; do
    printf '%s: ' "$module"; pkg-config --modversion "$module"
  done
} > "$source_bundle/configuration/build.txt"
(cd "$prefix" && sha256sum lib/libass.so.9 lib/liblcms2.so.2) > "$source_bundle/runtime.sha256"
cat > "$source_bundle/README.txt" <<'MANIFEST'
libass 0.17.5 (ISC) and LittleCMS 2.20 core (MIT), unmodified upstream source.
Sources and their official release-asset SHA-256 digests are recorded in
media-support-sources.sha256. Both complete upstream archives are included,
along with their top-level license texts and the exact build script.

Reproduce on Debian 13: bash build_media_support.sh /absolute/prefix
Required: C compiler, make, pkgconf, curl, tar with xz, sha256sum, readelf,
Python 3, Meson and Ninja (CI uses the verified build-tools SDK). Install
libfreetype-dev, libfontconfig1-dev, libfribidi-dev, libharfbuzz-dev,
libunibreak-dev and nasm on x86. These separately distributed OS libraries
remain Debian 13 packages; retain their distributor notices and sources.
No CPU-specific compiler flags are added. Build on the intended oldest
runtime baseline, not a newer distribution, for compatible glibc requirements.

libass uses its generated Autotools configure and export-symbol list because
upstream's Meson shared build warns it is not suitable for distribution.
Fontconfig, FreeType, FriBidi, HarfBuzz, libunibreak and assembly stay enabled.
LittleCMS uses Meson, with core shared library and its upstream tests enabled.
Its optional GPL fastfloat/threaded plugins and command-line utilities remain
disabled. No Meson subproject download is permitted. The build runs libass's
make check and LittleCMS's meson test; retained logs distinguish actual tests.
External libass visual regression samples are not fetched by this script.

configuration/ retains configure flags, compiler inputs, dependency versions,
Autotools configuration, Meson options/dependencies, and upstream test logs.
runtime.sha256 identifies the installed libass.so.9 and liblcms2.so.2 bytes
before deployment tools rewrite ELF paths. Verify these against the selected
prefix before copying, and verify the application resolves both libraries
from that prefix rather than accidentally deploying distribution libraries.

For downstream builds, prepend PREFIX/lib/pkgconfig to PKG_CONFIG_PATH and
PREFIX/lib to LD_LIBRARY_PATH. Source bundle location:
PREFIX/share/niconeon/media-support-source (NICONEON_MEDIA_SUPPORT_SOURCE_DIR).
MANIFEST
# Record the remaining retained evidence too, so later corruption fails closed.
(
  cd "$source_bundle"
  find configuration notices -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum
  sha256sum build_media_support.sh README.txt
) > "$source_bundle/evidence.sha256"
printf 'Installed libass 0.17.5 and LittleCMS 2.20 with corresponding source into %s\n' "$prefix"
