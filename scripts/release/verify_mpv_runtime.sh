#!/usr/bin/env bash
set -euo pipefail
[[ $# -eq 1 ]] || { echo "usage: $0 <libmpv-runtime>" >&2; exit 2; }
source_dir="${NICONEON_MPV_SOURCE_DIR:-}"
if [[ -z "$source_dir" ]] && command -v pkg-config >/dev/null 2>&1; then
  source_dir="$(pkg-config --variable=prefix mpv)/share/niconeon/libmpv-source"
fi
[[ -f "$source_dir/library.sha256" && -f "$1" ]] || { echo "missing supported libmpv build/runtime" >&2; exit 1; }
read -r expected _ < "$source_dir/library.sha256"
actual="$(sha256sum "$1" | cut -d ' ' -f 1)"
[[ "$actual" == "$expected" ]] || { echo "runtime does not match the patched libmpv build; check loader/SDK paths" >&2; exit 1; }
echo "Patched libmpv runtime matches its corresponding source build"
