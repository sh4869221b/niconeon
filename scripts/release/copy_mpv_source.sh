#!/usr/bin/env bash
set -euo pipefail
[[ $# -eq 1 ]] || { echo "usage: $0 <license-directory>" >&2; exit 2; }
source_dir="${NICONEON_MPV_SOURCE_DIR:-}"
if [[ -z "$source_dir" ]] && command -v pkg-config >/dev/null 2>&1; then
  source_dir="$(pkg-config --variable=prefix mpv)/share/niconeon/libmpv-source"
fi
for file in mpv-v0.42.0.tar.gz mpv-static-libplacebo.patch build_mpv.sh Copyright README.txt library.sha256; do
  [[ -f "$source_dir/$file" ]] || { echo "missing supported libmpv corresponding source: $file; run scripts/deps/build_mpv.sh first" >&2; exit 1; }
done
printf '%s  %s\n' bcfebbfa2b65a6d88dc97ea18117831072373e38b5785a2afb874ed4df5070dd "$source_dir/mpv-v0.42.0.tar.gz" | sha256sum --check --status
mkdir -p "$1/libmpv-source"
for file in mpv-v0.42.0.tar.gz mpv-static-libplacebo.patch build_mpv.sh Copyright README.txt library.sha256; do
  cp "$source_dir/$file" "$1/libmpv-source/"
done
