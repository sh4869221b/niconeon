#!/usr/bin/env bash
# Run before linuxdeploy/patchelf changes SDK runtime bytes.
set -euo pipefail
[[ $# -eq 2 ]] || { echo "usage: $0 <license-directory> <qt-sdk-prefix>" >&2; exit 2; }
prefix="$2"
source_dir="${NICONEON_QT_SOURCE_DIR:-$prefix/share/niconeon/qt-source}"
script_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
manifest="$script_dir/../deps/qt-sources.sha256"
for file in qt-sources.sha256 build_qt.sh README.txt runtime.sha256; do
  [[ -f "$source_dir/$file" ]] || { echo "missing Qt corresponding source: $file" >&2; exit 1; }
done
cmp "$manifest" "$source_dir/qt-sources.sha256"
(cd "$source_dir" && sha256sum --check "$manifest")
for module in qtbase qtshadertools qtsvg qtdeclarative; do
  [[ -f "$source_dir/notices/$module/LICENSES/GPL-3.0-only.txt" ]] || {
    echo "missing Qt module license texts: $module" >&2; exit 1;
  }
  [[ -f "$source_dir/configuration/$module-CMakeCache.txt" ]] || {
    echo "missing Qt module build configuration: $module" >&2; exit 1;
  }
done
# The runtime manifest must stay within the SDK, contain actual shared libraries,
# and cover at least the QtCore/QtQuick libraries and SQLite/XCB plugins.
python3 - "$source_dir/runtime.sha256" <<'PY'
from pathlib import Path, PurePosixPath
import re
import sys

lines = Path(sys.argv[1]).read_text().splitlines()
paths = []
for line in lines:
    match = re.fullmatch(r"[0-9a-f]{64}  ((?:lib|plugins|qml)/[^\\\n]+)", line)
    if not match or ".." in PurePosixPath(match[1]).parts or ".so" not in match[1]:
        sys.exit("invalid Qt runtime manifest")
    paths.append(match[1])
for required in ("lib/libQt6Core.so.6.12.0", "lib/libQt6Quick.so.6.12.0",
                 "plugins/sqldrivers/libqsqlite.so", "plugins/platforms/libqxcb.so"):
    if required not in paths:
        sys.exit(f"Qt runtime manifest omits {required}")
PY
(cd "$prefix" && sha256sum --check "$source_dir/runtime.sha256")
mkdir -p "$1/qt-source"
cp -R "$source_dir/." "$1/qt-source/"
