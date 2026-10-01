#!/usr/bin/env bash
set -euo pipefail
[[ $# -eq 1 ]] || { echo "usage: $0 <license-directory>" >&2; exit 2; }
source_dir="${NICONEON_MPV_SOURCE_DIR:-}"
if [[ -z "$source_dir" ]] && command -v pkg-config >/dev/null 2>&1; then
  source_dir="$(pkg-config --variable=prefix mpv)/share/niconeon/libmpv-source"
fi
for file in mpv-v0.41.0.tar.gz mpv-lut-padding.patch build_mpv.sh Copyright README.txt library.sha256; do
  [[ -f "$source_dir/$file" ]] || { echo "missing patched libmpv corresponding source: $file; run scripts/deps/build_mpv.sh first" >&2; exit 1; }
done
printf '%s  %s\n' ee21092a5ee427353392360929dc64645c54479aefdb5babc5cfbb5fad626209 "$source_dir/mpv-v0.41.0.tar.gz" | sha256sum --check --status
mkdir -p "$1/libmpv-source"
for file in mpv-v0.41.0.tar.gz mpv-lut-padding.patch build_mpv.sh Copyright README.txt library.sha256; do
  cp "$source_dir/$file" "$1/libmpv-source/"
done
