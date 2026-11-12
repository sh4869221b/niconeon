#!/usr/bin/env bash
# Run before linuxdeploy/patchelf rewrites the selected SDK runtime bytes.
set -euo pipefail
[[ $# -eq 2 && "$2" == /* ]] || { echo "usage: $0 <license-directory> <absolute-media-prefix>" >&2; exit 2; }
prefix="$(cd "$2" && pwd)"
source_dir="${NICONEON_MEDIA_SUPPORT_SOURCE_DIR:-$prefix/share/niconeon/media-support-source}"
source_dir="$(cd "$source_dir" && pwd)"
script_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
manifest="$script_dir/../deps/media-support-sources.sha256"
for file in libass-0.17.5.tar.xz lcms2-2.20.tar.gz media-support-sources.sha256 \
  build_media_support.sh README.txt runtime.sha256 evidence.sha256 \
  notices/libass/COPYING notices/lcms2/LICENSE configuration/build.txt \
  configuration/libass/config.log configuration/libass/config.status \
  configuration/libass/config.h configuration/libass/Makefile \
  configuration/lcms2/intro-buildoptions.json configuration/lcms2/intro-dependencies.json \
  configuration/lcms2/compile_commands.json configuration/lcms2/meson-log.txt \
  configuration/lcms2/testlog.txt; do
  [[ -f "$source_dir/$file" ]] || { echo "missing media support source/evidence: $file" >&2; exit 1; }
done
cmp "$manifest" "$source_dir/media-support-sources.sha256"
(cd "$source_dir" && sha256sum --check "$manifest")
# Parse strictly before sha256sum: a corrupted manifest must not read outside
# its bundle/prefix, omit a runtime, or add an unrelated path.
python3 - "$source_dir" <<'PYTHON'
from pathlib import Path, PurePosixPath
import re
import sys

bundle = Path(sys.argv[1])
runtime_paths = []
for line in (bundle / "runtime.sha256").read_text().splitlines():
    match = re.fullmatch(r"[0-9a-f]{64}  (lib/libass\.so\.9|lib/liblcms2\.so\.2)", line)
    if not match:
        sys.exit("invalid media support runtime manifest")
    runtime_paths.append(match[1])
if sorted(runtime_paths) != ["lib/libass.so.9", "lib/liblcms2.so.2"]:
    sys.exit("media support manifest must cover exactly both runtimes")

evidence_paths = []
for line in (bundle / "evidence.sha256").read_text().splitlines():
    match = re.fullmatch(r"[0-9a-f]{64}  ([^\\\n]+)", line)
    if not match:
        sys.exit("invalid media support evidence manifest")
    path = PurePosixPath(match[1])
    if path.is_absolute() or ".." in path.parts:
        sys.exit("unsafe media support evidence path")
    if path.parts[0] not in ("configuration", "notices") and str(path) not in (
        "build_media_support.sh", "README.txt"
    ):
        sys.exit("unexpected media support evidence path")
    if not (bundle / path).resolve().is_relative_to(bundle.resolve()):
        sys.exit("media support evidence escapes its bundle")
    evidence_paths.append(str(path))
expected = sorted(["build_media_support.sh", "README.txt"] + [
    str(path.relative_to(bundle))
    for directory in ("configuration", "notices")
    for path in (bundle / directory).rglob("*") if path.is_file()
])
if sorted(evidence_paths) != expected:
    sys.exit("media support evidence manifest does not cover the complete bundle")
PYTHON
(cd "$source_dir" && sha256sum --check evidence.sha256)
for soname in libass.so.9 liblcms2.so.2; do
  runtime="$prefix/lib/$soname"
  [[ -f "$runtime" ]] || { echo "missing media support runtime: $runtime" >&2; exit 1; }
  case "$(readlink -f "$runtime")" in
    "$prefix"/lib/*) ;;
    *) echo "media support runtime resolves outside its prefix: $runtime" >&2; exit 1 ;;
  esac
  actual="$(LC_ALL=C readelf -d "$runtime" | sed -n 's/.*(SONAME).*\[\([^]]*\)\].*/\1/p')"
  [[ "$actual" == "$soname" ]] || { echo "unexpected media support runtime ABI: $actual" >&2; exit 1; }
done
(cd "$prefix" && sha256sum --check "$source_dir/runtime.sha256")
mkdir -p "$1/media-support-source"
cp -R "$source_dir/." "$1/media-support-source/"
