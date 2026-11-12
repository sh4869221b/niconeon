#!/usr/bin/env bash
# Build PIC libplacebo for private static linkage into the Linux libmpv.
set -euo pipefail
[[ $# -eq 1 ]] || { echo "usage: $0 <absolute-install-prefix>" >&2; exit 2; }
prefix="$1"
[[ "$prefix" == /* ]] || { echo "prefix must be absolute" >&2; exit 2; }
[[ "$(uname -s)" == Linux ]] || { echo "this source build is for Linux" >&2; exit 2; }
script_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
work="${NICONEON_LIBPLACEBO_WORK_DIR:-${script_dir}/../../build/_deps/libplacebo}"
mkdir -p "$work" "$prefix"
work="$(cd "$work" && pwd)"
prefix="$(cd "$prefix" && pwd)"
archive="$work/libplacebo-v7.387.0.tar.gz"
checksum=f9e62d3774a9c4cfd198ec3abaa88294aaade2e31c04d5ab02bcf4abc98b1df5
url=https://github.com/haasn/libplacebo/archive/refs/tags/v7.387.0.tar.gz
if [[ ! -f "$archive" ]]; then
  curl --fail --location --retry 2 --connect-timeout 30 --max-time 180 "$url" -o "$archive.download"
  printf '%s  %s\n' "$checksum" "$archive.download" | sha256sum --check --status
  mv "$archive.download" "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status
# Extract afresh so a modified previous build cannot be labelled pristine source.
run_dir="$(mktemp -d "$work/build-7.387.0.XXXXXX")"
tar -xzf "$archive" -C "$run_dir"
source_dir="$run_dir/libplacebo-7.387.0"
meson="${NICONEON_MESON:-meson}"
"$meson" setup "$run_dir/build" "$source_dir" --prefix "$prefix" --libdir lib \
  --buildtype release --wrap-mode nodownload -Ddefault_library=static -Db_staticpic=true \
  -Ddemos=false -Dvulkan=enabled -Dvk-proc-addr=enabled \
  -Dopengl=enabled -Dgl-proc-addr=enabled -Dshaderc=enabled -Dlcms=enabled
"$meson" compile -C "$run_dir/build" -j "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
"$meson" install -C "$run_dir/build"
static_library="$prefix/lib/libplacebo.a"
[[ -f "$static_library" ]] || { echo "installed libplacebo static archive was not found" >&2; exit 1; }
source_bundle="$prefix/share/niconeon/libplacebo-source"
mkdir -p "$source_bundle/notices"
cp "$archive" "$script_dir/build_libplacebo.sh" "$source_dir/LICENSE" "$source_bundle/"
# Preserve whole files with embedded third-party notices, not truncated headers.
# The verified archive also retains every upstream notice in its original location.
cp "$source_dir/3rdparty/json.h" "$source_bundle/notices/"
for file in src/hash.h src/dither.c src/filters.c src/convert.cc \
  src/shaders/dithering.c src/shaders/deinterlacing.c src/shaders/film_grain_av1.c \
  src/include/libplacebo/shaders/deinterlacing.h src/include/libplacebo/shaders/film_grain.h; do
  mkdir -p "$source_bundle/notices/$(dirname "$file")"
  cp "$source_dir/$file" "$source_bundle/notices/$file"
done
# glad emits Khronos notices in the generated headers. fast_float is a system
# header-only dependency on Debian; retain its distributor-provided license too.
mkdir -p "$source_bundle/notices/glad"
cp -R "$run_dir/build/src/opengl/include/glad/." "$source_bundle/notices/glad/"
[[ -f /usr/share/doc/libfast-float-dev/copyright ]] || {
  echo "missing libfast-float-dev copyright; install the Debian development package" >&2; exit 1;
}
cp /usr/share/doc/libfast-float-dev/copyright "$source_bundle/notices/fast-float-copyright"
printf '%s  %s\n' "$(sha256sum "$static_library" | cut -d ' ' -f 1)" "$(basename "$static_library")" > "$source_bundle/library.sha256"
cat > "$source_bundle/README.txt" <<MANIFEST
libplacebo 7.387.0, unmodified upstream source (LGPL-2.1-or-later)
Source: $url
Source SHA-256: $checksum
Build on Debian 13: bash build_libplacebo.sh /absolute/prefix
Required: C/C++ compiler, Meson >=1.3, Ninja, pkgconf, curl, tar, sha256sum,
python3-glad >=2.0, python3-jinja2, libfast-float-dev, libvulkan-dev
(Vulkan 1.4 headers and matching vk.xml), libshaderc-dev >=2019.1,
liblcms2-dev >=2.9. Use distribution development packages and their sources
for these separately distributed dependencies; upstream submodules are not
included in this release archive. Meson downloads are disabled.
Vulkan, OpenGL, their loaders, shaderc and LittleCMS are explicitly required.
Demos are disabled; other upstream feature defaults are retained.
The archive and build script reproduce this unmodified upstream library.
LICENSE and notices/ retain upstream and generated third-party notices.
library.sha256 identifies the installed PIC static archive libplacebo.a.
This archive is incorporated with hidden symbols into the separately
hash-verified libmpv runtime. No separate libplacebo.so.387 is distributed;
this avoids exporting competing symbols alongside FFmpeg's system libplacebo.
The libmpv source bundle supplies its build script and final runtime hash.
MANIFEST
printf 'Installed libplacebo and corresponding source into %s\n' "$prefix"
