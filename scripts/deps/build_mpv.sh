#!/usr/bin/env bash
# Build the supported libmpv dependency, retaining its complete corresponding source.
set -euo pipefail
if [[ $# -ne 1 ]]; then
  echo "usage: $0 <absolute-install-prefix>" >&2
  exit 2
fi
prefix="$1"
[[ "$prefix" == /* || "$prefix" =~ ^[A-Za-z]:[/\\] ]] || { echo "prefix must be absolute" >&2; exit 2; }
script_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
work="${NICONEON_MPV_WORK_DIR:-${script_dir}/../../build/_deps/mpv}"
mkdir -p "$work"
work="$(cd "$work" && pwd)"
archive="$work/mpv-v0.42.0.tar.gz"
checksum=bcfebbfa2b65a6d88dc97ea18117831072373e38b5785a2afb874ed4df5070dd
url=https://github.com/mpv-player/mpv/archive/refs/tags/v0.42.0.tar.gz
if [[ ! -f "$archive" ]]; then
  curl --fail --location --retry 2 --connect-timeout 30 --max-time 180 "$url" -o "$archive.download"
  printf '%s  %s\n' "$checksum" "$archive.download" | sha256sum --check --status
  mv "$archive.download" "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status
# The upstream scaler LUT fix is included. Keep Linux libplacebo symbols private.
source_dir="$work/mpv-0.42.0"
if [[ ! -f "$source_dir/.niconeon-static-placebo-patched" ]]; then
  [[ ! -e "$source_dir" ]] || { echo "unmarked source directory already exists: $source_dir" >&2; exit 1; }
  tar -xzf "$archive" -C "$work"
  patch --batch --fuzz=0 -p1 -d "$source_dir" < "$script_dir/mpv-static-libplacebo.patch"
  touch "$source_dir/.niconeon-static-placebo-patched"
fi
meson="${NICONEON_MESON:-meson}"
"$meson" setup "$work/build-0.42.0" "$source_dir" --prefix "$prefix" --libdir lib \
  --buildtype release --wrap-mode nodownload -Dlibmpv=true -Dcplayer=false \
  -Dbuild-date=false -Dplain-gl=enabled -Dmanpage-build=disabled
"$meson" compile -C "$work/build-0.42.0" -j "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
"$meson" install -C "$work/build-0.42.0"
source_bundle="$prefix/share/niconeon/libmpv-source"
mkdir -p "$source_bundle"
cp "$archive" "$script_dir/mpv-static-libplacebo.patch" "$script_dir/build_mpv.sh" "$source_bundle/"
cp "$source_dir/Copyright" "$source_bundle/"
if [[ -f "$prefix/bin/libmpv-2.dll" ]]; then
  runtime="$prefix/bin/libmpv-2.dll"
else
  runtime="$prefix/lib/libmpv.so.2"
fi
[[ -f "$runtime" ]] || { echo "installed libmpv runtime was not found" >&2; exit 1; }
if [[ "$(uname -s)" == Linux ]]; then
  # FFmpeg may load an older libplacebo ABI. No new pl_* symbol may escape
  # libmpv or remain dynamically unresolved to another libplacebo.
  symbols="$(nm -D "$runtime")"
  if grep -Eq '[[:space:]]pl_[[:alnum:]_]+' <<< "$symbols"; then
    echo "Linux libplacebo symbols are not isolated inside libmpv" >&2
    exit 1
  fi
fi
printf '%s  %s\n' "$(sha256sum "$runtime" | cut -d ' ' -f 1)" "$(basename "$runtime")" > "$source_bundle/library.sha256"
cat > "$source_bundle/README.txt" <<MANIFEST
libmpv 0.42.0 (includes upstream scaler LUT padding fix)
Source: $url
Source SHA-256: $checksum
Included upstream fix: https://github.com/mpv-player/mpv/commit/72d43dc9c999a21d867cdc0f934f3e4cd2195aa9
Patch: mpv-static-libplacebo.patch (Linux static dependency isolation)
Build: bash build_mpv.sh /absolute/prefix (Meson, Ninja, C compiler, pkgconf,
FFmpeg, libass and libplacebo development packages are required).
The included source archive, patch and build script are the corresponding
source for this library. Linux libplacebo is statically linked with hidden symbols. No interpolation-quality setting is changed.
MANIFEST
printf 'Installed libmpv and corresponding source into %s\n' "$prefix"
