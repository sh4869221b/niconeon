#!/usr/bin/env bash
# Rebuild the official UCRT64 recipe, retaining platform patches and features.
set -euo pipefail
[[ ${MSYSTEM:-} == UCRT64 && ${MINGW_PREFIX:-} == /ucrt64 ]] || { echo 'UCRT64 shell required' >&2; exit 1; }
for package in mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-declarative; do
  [[ $(pacman -Q "$package") == "$package 6.12.0-1" ]] || {
    echo "Review Qt recipe/security pins before replacing $package (expected 6.12.0-1)" >&2; exit 1;
  }
done
here=$(cd "$(dirname "$0")" && pwd)
work=${NICONEON_WINDOWS_QT_WORK_DIR:-"$here/../../build/windows-qt"}
mkdir -p "$work"
work=$(cd "$work" && pwd)
source_dir="$work/source"
recipe="$work/recipe"
commit=86b6d2d5ff436fcc457cd729c398829410d013c0
inventory=$(pacman -Q | LC_ALL=C sort | sha256sum | cut -d' ' -f1)
identity=$(cat "$0" "$here/windows-qt-recipe.sha256" "$here/qt-sources.sha256" | sha256sum | cut -d' ' -f1)
fetch() { curl --fail --location --retry 3 --output "$2" "$1"; }
if [[ -f "$work/complete" ]]; then
  [[ $(cat "$work/complete") == "$identity $inventory" ]] || { echo 'Qt cache inventory/script mismatch: use a new cache directory' >&2; exit 1; }
  (cd "$work" && sha256sum --check packages.sha256)
else
  # Never reuse an interrupted extraction or a partially packaged attempt.
  # Only fully verified outputs are promoted into the stable cache paths below.
  attempt=$(mktemp -d "$work/attempt.XXXXXX")
  recipe="$attempt/recipe"
  source_dir="$attempt/source"
  package_dir="$attempt/packages"
  mkdir -p "$recipe" "$source_dir/recipe" "$package_dir"
  # Git autocrlf may affect checksum manifests on Windows. Normalize only
  # trailing CR, then use the same normalized bytes for downloads and checks.
  sed 's/\r$//' "$here/windows-qt-recipe.sha256" > "$source_dir/windows-qt-recipe.sha256"
  sed 's/\r$//' "$here/qt-sources.sha256" | grep -E '  (qtbase-everywhere|CVE-)' > "$source_dir/qt-sources.sha256"
  while read -r digest name; do
    [[ -n $name ]] || continue
    fetch "https://raw.githubusercontent.com/msys2/MINGW-packages/$commit/mingw-w64-qt6-base/$name" "$recipe/$name"
  done < "$source_dir/windows-qt-recipe.sha256"
  (cd "$recipe" && sha256sum --check "$source_dir/windows-qt-recipe.sha256")
  cp "$recipe/PKGBUILD" "$source_dir/PKGBUILD.upstream"
  while read -r digest name; do
    case "$name" in
      CVE-*) url="https://download.qt.io/official_releases/qt/6.12/$name" ;;
      *) url="https://download.qt.io/official_releases/qt/6.12/6.12.0/submodules/$name" ;;
    esac
    fetch "$url" "$recipe/$name"
  done < "$source_dir/qt-sources.sha256"
  (cd "$recipe" && sha256sum --check "$source_dir/qt-sources.sha256")
  # Extend, rather than reimplement, the exact upstream preparation function.
  cat >> "$recipe/PKGBUILD" <<'PKGBUILD'

# niconeon: official Qt security fixes; no public ABI or feature changes.
eval "$(declare -f prepare | sed '1s/prepare/prepare_msys2_original/')"
source+=(CVE-2026-105924-qtbase-6.12.diff CVE-2026-116337-qtbase-6.12-0001.diff CVE-2026-116337-qtbase-6.12-0002.diff)
sha256sums+=(c27d27b86aaf2cdab0c18d513fbb94a432e98782eddfc5f856387e4be68be50b 74c5028985dcc0455255909de4dc80f50cf5472dbf465ce46ebc791a7b080f12 83abc89d507dbd9a94dc5718b0125492c1c37b0609a8f404a1f164925940faec)
prepare() {
  prepare_msys2_original
  cd "$srcdir/$_pkgfn"
  # Upstream 0001 tests depend on 0002; numbering is not apply order.
  for security_patch in CVE-2026-105924-qtbase-6.12.diff CVE-2026-116337-qtbase-6.12-0002.diff CVE-2026-116337-qtbase-6.12-0001.diff; do
    patch --batch --forward --fuzz=0 -p1 < "$srcdir/$security_patch"
  done
}
PKGBUILD
  cp "$recipe/PKGBUILD" "$source_dir/PKGBUILD"
  cp "$recipe/"*.patch "$source_dir/recipe/"
  cp "$source_dir/PKGBUILD.upstream" "$source_dir/recipe/PKGBUILD"
  cp "$recipe/"*.diff "$recipe/"*.tar.xz "$source_dir/"
  cp "$0" "$source_dir/build_windows_qt.sh"
  pacman -Q | LC_ALL=C sort > "$source_dir/package-inventory.txt"
  (cd "$recipe" && MINGW_ARCH=ucrt64 PKGDEST="$package_dir" makepkg-mingw --noconfirm --force) 2>&1 | tee "$source_dir/build.log"
  cp -R "$recipe/src/qtbase-everywhere-src-6.12.0/LICENSES" "$source_dir/"
  cp "$recipe/src/build-UCRT64/CMakeCache.txt" "$source_dir/CMakeCache.txt"
  for directory in source packages; do
    if [[ -e "$work/$directory" ]]; then
      mv "$work/$directory" "$attempt/previous-$directory"
    fi
    mv "$attempt/$directory" "$work/$directory"
  done
  source_dir="$work/source"
  (cd "$work" && sha256sum packages/*.pkg.tar.* > packages.sha256)
fi
# Always reinstall the verified package, including on a cache hit. No --needed.
mapfile -t packages < <(find "$work/packages" -maxdepth 1 -name 'mingw-w64-ucrt-x86_64-qt6-base-6.12.0-1-*.pkg.tar.*' ! -name '*.sig' | sort)
[[ ${#packages[@]} == 1 ]] || { echo 'expected exactly one QtBase binary package' >&2; exit 1; }
pacman --noconfirm -U "${packages[0]}"
[[ $(pacman -Q mingw-w64-ucrt-x86_64-qt6-base) == 'mingw-w64-ucrt-x86_64-qt6-base 6.12.0-1' ]]
if [[ -f "$work/complete" ]]; then
  (cd /ucrt64 && sha256sum --check "$source_dir/runtime.sha256")
else
  # Includes Base plugins as well as Qt6Core/Network and other Base DLLs.
  pacman -Qlq mingw-w64-ucrt-x86_64-qt6-base | grep -E '^/ucrt64/.*\.dll$' | LC_ALL=C sort | while read -r dll; do
    (cd /ucrt64 && sha256sum "${dll#/ucrt64/}")
  done > "$source_dir/runtime.sha256"
  grep -q '  bin/Qt6Core.dll$' "$source_dir/runtime.sha256"
  grep -q '  bin/Qt6Network.dll$' "$source_dir/runtime.sha256"
  cp "$work/packages.sha256" "$source_dir/"
  cat > "$source_dir/README.txt" <<README
QtBase 6.12.0-1 UCRT64 rebuilt from MSYS2 recipe $commit.
Official recipe: https://github.com/msys2/MINGW-packages/tree/$commit/mingw-w64-qt6-base
All upstream MSYS2 patches/features retained. Three official Qt patches fix
CVE-2026-105924 and CVE-2026-116337. Patches change QtCore CBOR/JSON conversion
and private QtNetwork HTTP parsing, not public ABI. Existing same-version
QtDeclarative/Tools packages are retained; Windows integration tests must pass.
Source, original/modified recipes, patches, licenses, build log, toolchain
inventory and installed Base DLL hashes are retained here. Package archives
are in the build cache alongside this source directory. This is a local
security rebuild, not an official MSYS2 published binary.
README
  (cd "$source_dir" && find . -type f ! -name evidence.sha256 -print0 | LC_ALL=C sort -z | xargs -0 sha256sum > evidence.sha256)
  printf '%s %s\n' "$identity" "$inventory" > "$work/complete"
fi
(cd "$source_dir" && sha256sum --check evidence.sha256)
mkdir -p /ucrt64/share/niconeon/windows-qt-source
cp -R "$source_dir/." /ucrt64/share/niconeon/windows-qt-source/
