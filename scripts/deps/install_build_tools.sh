#!/usr/bin/env bash
# Install reviewed upstream Linux build tools without changing system packages.
set -euo pipefail
[[ $# -eq 1 && "$1" == /* ]] || { echo "usage: $0 <absolute-install-prefix>" >&2; exit 2; }
[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || { echo "these tool archives require Linux x86_64" >&2; exit 2; }
prefix="$1"
mkdir -p "$prefix/bin" "$prefix/downloads" "$prefix/cmake" "$prefix/meson" "$prefix/llvm"
fetch() {
  local name="$1" url="$2" checksum="$3" archive="$prefix/downloads/$1"
  if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 2 --connect-timeout 30 --max-time 900 "$url" -o "$archive.download"
    printf '%s  %s\n' "$checksum" "$archive.download" | sha256sum --check --status
    mv "$archive.download" "$archive"
  fi
  printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status
}
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
for tool in cmake ctest cpack; do
  ln -sfn "../cmake/bin/$tool" "$prefix/bin/$tool"
done
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

# Official release binaries link only to the Debian system runtime. Keep their
# matching Clang builtin headers for clang-tidy, but not the full compiler SDK.
fetch LLVM-23.1.5-Linux-X64.tar.xz \
  https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.5/LLVM-23.1.5-Linux-X64.tar.xz \
  f72edb0b36580ece7f52e23858f00c18fad4e87395ea7ad05d7e55c39f8fc78a
python3 - "$prefix/downloads/LLVM-23.1.5-Linux-X64.tar.xz" "$prefix/llvm" <<'PYTHON'
import pathlib
import sys
import tarfile

archive, destination = sys.argv[1:]
root = "LLVM-23.1.5-Linux-X64/"
with tarfile.open(archive, mode="r|xz") as source:
    for member in source:
        if not member.name.startswith(root):
            continue
        relative = member.name[len(root):]
        name = pathlib.PurePosixPath(relative).name.upper()
        wanted = relative in ("bin/clang-format", "bin/clang-tidy")
        wanted |= relative.startswith("lib/clang/23/include/")
        wanted |= member.isfile() and (name.startswith("LICENSE") or name.startswith("COPYING"))
        if wanted:
            member.name = relative
            source.extract(member, destination, filter="data")
for required in ("bin/clang-format", "bin/clang-tidy", "lib/clang/23/include/stddef.h"):
    if not (pathlib.Path(destination) / required).is_file():
        raise SystemExit(f"LLVM archive lacks required file: {required}")
PYTHON
# The binary archive's support-library notice refers to the full LLVM license.
# Retain the exact release's complete LLVM and Clang license documents as well.
fetch llvm-23.1.5-LICENSE.TXT \
  https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-23.1.5/llvm/LICENSE.TXT \
  8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee
fetch clang-23.1.5-LICENSE.TXT \
  https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-23.1.5/clang/LICENSE.TXT \
  ebcd9bbf783a73d05c53ba4d586b8d5813dcdf3bbec50265860ccc885e606f47
mkdir -p "$prefix/llvm/licenses"
cp "$prefix/downloads/llvm-23.1.5-LICENSE.TXT" "$prefix/downloads/clang-23.1.5-LICENSE.TXT" "$prefix/llvm/licenses/"
for tool in clang-format clang-tidy; do
  ln -sfn "../llvm/bin/$tool" "$prefix/bin/$tool"
  ln -sfn "$tool" "$prefix/bin/$tool-23"
  "$prefix/bin/$tool" --version
done
# clang-tidy resolves builtin headers relative to the actual binary directory.
# A real C++ parse catches missing resource headers that --version cannot find.
probe="$(mktemp --suffix=.cpp)"
printf '#include <stddef.h>\n#include <vector>\nint main() { std::vector<int> v; return static_cast<int>(v.size()); }\n' > "$probe"
"$prefix/bin/clang-tidy" --config='{Checks: "clang-analyzer-*"}' "$probe" -- -std=c++23
rm -f "$probe"
# This 2 GB archive is a build-tool input, not application corresponding source.
# Exclude it from the published SDK artifact after successful extraction/checks.
rm -f "$prefix/downloads/LLVM-23.1.5-Linux-X64.tar.xz"
