#!/usr/bin/env bash
# Meson builds can probe CMake; keep all three tools identical to the Linux
# SDK pins, without downloading or unpacking its clang-format/clang-tidy SDK.
set -euo pipefail
[[ $# -eq 1 && "$1" == /* ]] || { echo "usage: $0 <absolute-install-prefix>" >&2; exit 2; }
[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || { echo "these tool archives require Linux x86_64" >&2; exit 2; }
prefix="$1"
mkdir -p "$prefix/bin" "$prefix/downloads" "$prefix/cmake" "$prefix/meson"
fetch() {
  local name="$1" url="$2" checksum="$3" archive="$prefix/downloads/$1"
  if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 2 --connect-timeout 30 --max-time 900 "$url" -o "$archive.download"
    printf '%s  %s\n' "$checksum" "$archive.download" | sha256sum --check --status
    mv "$archive.download" "$archive"
  fi
  printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status
}
# Keep these tuples in sync with install_build_tools.sh (covered by a test).
fetch cmake-4.4.5-linux-x86_64.tar.gz \
  https://github.com/Kitware/CMake/releases/download/v4.4.5/cmake-4.4.5-linux-x86_64.tar.gz \
  ed85135c5a5135b45e50423cb406dea04ca3338da0b8ef3c9ede963559680be0
fetch ninja-1.13.2-linux.zip \
  https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip \
  5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6
fetch meson-1.12.1.tar.gz \
  https://github.com/mesonbuild/meson/releases/download/1.12.1/meson-1.12.1.tar.gz \
  ab0a6ca09f8ef70c564c8241fb5a23957886a0b53fb58412b5e07eaf07dba743
tar -xzf "$prefix/downloads/cmake-4.4.5-linux-x86_64.tar.gz" --strip-components=1 -C "$prefix/cmake"
unzip -oq "$prefix/downloads/ninja-1.13.2-linux.zip" -d "$prefix/bin"
tar -xzf "$prefix/downloads/meson-1.12.1.tar.gz" --strip-components=1 -C "$prefix/meson"
ln -sfn ../cmake/bin/cmake "$prefix/bin/cmake"
cat > "$prefix/bin/meson" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
exec python3 "$root/meson/meson.py" "$@"
WRAPPER
chmod +x "$prefix/bin/meson" "$prefix/bin/ninja"
"$prefix/bin/cmake" --version
"$prefix/bin/ninja" --version
"$prefix/bin/meson" --version
