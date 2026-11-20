#!/usr/bin/env bash
# Temporary diagnostic: never upload the cache or use these binaries for release.
set -euo pipefail
[[ ${MSYSTEM:-} == UCRT64 ]]
baseline="$PWD/build/windows-release"
test -s "$baseline/compile_commands.json"
test -s "$baseline/tests.xml"
stock_digest=$(sha256sum "$baseline/niconeon.exe")
build=$(mktemp -d "$PWD/build/windows-ccache-probe.XXXXXX")
state=$(mktemp -d "$(cygpath -u "$RUNNER_TEMP")/niconeon-ccache.XXXXXX")
helper="$PWD/scripts/ci/windows_ccache_probe.py"
pacman -Q | LC_ALL=C sort > "$state/toolchain.before"
compiler_digest=$(sha256sum /ucrt64/bin/c++.exe)

started=$SECONDS
pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-ccache
printf 'CCACHE_PROBE_PACKAGE_SECONDS=%s\n' "$((SECONDS - started))"
pacman -Q | LC_ALL=C sort > "$state/toolchain.after"
LC_ALL=C comm -23 "$state/toolchain.before" "$state/toolchain.after" > "$state/toolchain.changed"
test ! -s "$state/toolchain.changed"
test "$(sha256sum /ucrt64/bin/c++.exe)" = "$compiler_digest"
cache_tool=/ucrt64/bin/ccache.exe
tool_native=$(cygpath -am "$cache_tool")
printf 'CCACHE_PROBE_SOURCE=%s\n' "$(git rev-parse HEAD)"
pacman -Q mingw-w64-ucrt-x86_64-ccache mingw-w64-ucrt-x86_64-gcc
"$cache_tool" --version
printf 'CCACHE_PROBE_COMPILER=%s\n' "$compiler_digest"
printf 'compiler_check = content\nmax_size = 512M\n' > "$state/ccache.conf"
export CCACHE_CONFIGPATH
CCACHE_CONFIGPATH=$(cygpath -am "$state/ccache.conf")
export CCACHE_REMOTE_STORAGE=''
mkdir -p "$state"/{cold,warm}
export CCACHE_DIR
CCACHE_DIR=$(cygpath -am "$state/cold")

started=$SECONDS
cmake --preset windows-release -B "$build" "-DCMAKE_CXX_COMPILER_LAUNCHER=$tool_native"
printf 'CCACHE_PROBE_CONFIGURE_SECONDS=%s\n' "$((SECONDS - started))"
/usr/bin/python3 "$helper" commands "$baseline" "$build" "$tool_native"

measure() {
  local label=$1 started
  "$cache_tool" --zero-stats
  if [[ $label == warm ]]; then
    started=$SECONDS
    cmake --build "$build" --target clean
    printf 'CCACHE_PROBE_CLEAN_SECONDS=%s\n' "$((SECONDS - started))"
  fi
  started=$SECONDS
  cmake --build "$build" --parallel 4
  printf 'CCACHE_PROBE_BUILD_SECONDS phase=%s seconds=%s\n' "$label" "$((SECONDS - started))"
  "$cache_tool" --show-stats
  "$cache_tool" --print-stats > "$state/$label.stats"
  /usr/bin/python3 "$helper" snapshot "$build" "$state/$label.json"
  started=$SECONDS
  ctest --test-dir "$build" --no-tests=error --output-on-failure --output-junit "$state/$label.xml"
  printf 'CCACHE_PROBE_CTEST_SECONDS phase=%s seconds=%s\n' "$label" "$((SECONDS - started))"
}

measure cold
started=$SECONDS
tar -I 'gzip -1' -cf "$state/cache.tar.gz" -C "$state/cold" .
printf 'CCACHE_PROBE_ARCHIVE bytes=%s seconds=%s\n' "$(wc -c < "$state/cache.tar.gz")" "$((SECONDS - started))"
started=$SECONDS
tar -xzf "$state/cache.tar.gz" -C "$state/warm"
printf 'CCACHE_PROBE_LOCAL_RESTORE_SECONDS=%s\n' "$((SECONDS - started))"
CCACHE_DIR=$(cygpath -am "$state/warm")
measure warm
/usr/bin/python3 "$helper" commands "$baseline" "$build" "$tool_native"
/usr/bin/python3 "$helper" compare "$baseline" "$state"
for flavor in stock warm; do
  binary="$baseline/niconeon.exe"
  [[ $flavor == warm ]] && binary="$build/niconeon.exe"
  objdump -p "$binary" | awk '/^[[:space:]]*DLL Name:/ {print $3}' | LC_ALL=C sort > "$state/$flavor.imports"
  test -s "$state/$flavor.imports"
done
cmp "$state/stock.imports" "$state/warm.imports"
test "$(sha256sum "$baseline/niconeon.exe")" = "$stock_digest"
echo 'CCACHE_PROBE_STOCK_PE_IMPORTS_MATCH_AND_STOCK_UNCHANGED'
