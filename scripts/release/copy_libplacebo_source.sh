#!/usr/bin/env bash
set -euo pipefail
[[ $# -eq 2 ]] || { echo "usage: $0 <license-directory> <static-library-path>" >&2; exit 2; }
vulkan_archive=Vulkan-Headers-v1.4.368.tar.gz
source_dir="${NICONEON_LIBPLACEBO_SOURCE_DIR:-}"
if [[ -z "$source_dir" ]] && command -v pkg-config >/dev/null 2>&1; then
  source_dir="$(pkg-config --variable=prefix libplacebo)/share/niconeon/libplacebo-source"
fi
for file in libplacebo-v7.387.0.tar.gz build_libplacebo.sh LICENSE README.txt library.sha256 \
  "$vulkan_archive" notices/json.h notices/fast-float-copyright \
  notices/Vulkan-Headers/LICENSE.md notices/Vulkan-Headers/LICENSES/Apache-2.0.txt \
  notices/Vulkan-Headers/LICENSES/MIT.txt; do
  [[ -f "$source_dir/$file" ]] || { echo "missing supported libplacebo source: $file; run scripts/deps/build_libplacebo.sh first" >&2; exit 1; }
done
[[ -d "$source_dir/notices/glad" ]] || { echo "missing generated glad notices" >&2; exit 1; }
printf '%s  %s\n' f9e62d3774a9c4cfd198ec3abaa88294aaade2e31c04d5ab02bcf4abc98b1df5 "$source_dir/libplacebo-v7.387.0.tar.gz" | sha256sum --check --status
printf '%s  %s\n' 5c5c6f64fcce28a24cc934143a048707af036a858e4ed11d7f71fe016006ecda "$source_dir/$vulkan_archive" | sha256sum --check --status
[[ -f "$2" ]] || { echo "libplacebo static archive was not found: $2" >&2; exit 1; }
expected="$(cat "$source_dir/library.sha256")"
[[ "$expected" =~ ^([0-9a-f]{64})\ \ libplacebo\.a$ ]] || { echo "invalid libplacebo static archive manifest" >&2; exit 1; }
printf '%s  %s\n' "${BASH_REMATCH[1]}" "$2" | sha256sum --check --status || {
  echo "libplacebo static archive does not match its source bundle" >&2; exit 1;
}
mkdir -p "$1/libplacebo-source"
for file in libplacebo-v7.387.0.tar.gz "$vulkan_archive" build_libplacebo.sh LICENSE README.txt library.sha256; do
  cp "$source_dir/$file" "$1/libplacebo-source/"
done
cp -R "$source_dir/notices" "$1/libplacebo-source/"
