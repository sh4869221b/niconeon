#!/usr/bin/env bash
# Build the supported libmpv dependency, retaining its complete source and patch.
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
archive="$work/mpv-v0.41.0.tar.gz"
checksum=ee21092a5ee427353392360929dc64645c54479aefdb5babc5cfbb5fad626209
url=https://github.com/mpv-player/mpv/archive/refs/tags/v0.41.0.tar.gz
if [[ ! -f "$archive" ]]; then
  curl --fail --location --retry 2 --connect-timeout 30 --max-time 180 "$url" -o "$archive.download"
  printf '%s  %s\n' "$checksum" "$archive.download" | sha256sum --check --status
  mv "$archive.download" "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status
# Only this script's fresh source directory is used; never patch the system SDK.
source_dir="$work/mpv-0.41.0"
if [[ ! -f "$source_dir/.niconeon-lut-patched" ]]; then
  [[ ! -e "$source_dir" ]] || { echo "unmarked source directory already exists: $source_dir" >&2; exit 1; }
  tar -xzf "$archive" -C "$work"
  patch --batch --fuzz=0 -p1 -d "$source_dir" < "$script_dir/mpv-lut-padding.patch"
  touch "$source_dir/.niconeon-lut-patched"
fi
meson="${NICONEON_MESON:-meson}"
"$meson" setup "$work/build" "$source_dir" --prefix "$prefix" --libdir lib \
  --buildtype release --wrap-mode nodownload -Dlibmpv=true -Dcplayer=false \
  -Dbuild-date=false -Dplain-gl=enabled -Dmanpage-build=disabled
"$meson" compile -C "$work/build" -j "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
"$meson" install -C "$work/build"
source_bundle="$prefix/share/niconeon/libmpv-source"
mkdir -p "$source_bundle"
cp "$archive" "$script_dir/mpv-lut-padding.patch" "$script_dir/build_mpv.sh" "$source_bundle/"
cp "$source_dir/Copyright" "$source_bundle/"
if [[ -f "$prefix/bin/libmpv-2.dll" ]]; then
  runtime="$prefix/bin/libmpv-2.dll"
else
  runtime="$prefix/lib/libmpv.so.2"
fi
[[ -f "$runtime" ]] || { echo "installed libmpv runtime was not found" >&2; exit 1; }
printf '%s  %s\n' "$(sha256sum "$runtime" | cut -d ' ' -f 1)" "$(basename "$runtime")" > "$source_bundle/library.sha256"
cat > "$source_bundle/README.txt" <<MANIFEST
libmpv 0.41.0 with upstream scaler LUT padding fix
Source: $url
Source SHA-256: $checksum
Patch: https://github.com/mpv-player/mpv/commit/72d43dc9c999a21d867cdc0f934f3e4cd2195aa9
Build: bash build_mpv.sh /absolute/prefix (Meson, Ninja, C compiler, pkgconf,
FFmpeg, libass and libplacebo development packages are required).
The included source archive, patch and build script are the corresponding
source for this modified library. No interpolation-quality setting is changed.
MANIFEST
printf 'Installed patched libmpv and corresponding source into %s\n' "$prefix"
