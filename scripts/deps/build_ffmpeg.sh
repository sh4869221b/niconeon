#!/usr/bin/env bash
# Rebuild FFmpeg with the Debian baseline's features in an isolated prefix.
set -euo pipefail
[[ $# -eq 1 && "$1" == /* ]] || { echo "usage: $0 <absolute-install-prefix>" >&2; exit 2; }
[[ "$(uname -s)" == Linux ]] || { echo "this build is for Linux" >&2; exit 2; }
prefix="$1"
script_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
work="${NICONEON_FFMPEG_WORK_DIR:-$script_dir/../../build/_deps/ffmpeg}"
# Explicitly use the distribution executable, never a previously built copy.
baseline="${NICONEON_FFMPEG_BASELINE:-/usr/bin/ffmpeg}"
[[ -x "$baseline" ]] || { echo "install Debian ffmpeg and its build dependencies first" >&2; exit 1; }
mkdir -p "$work" "$prefix"
work="$(cd "$work" && pwd)"
prefix="$(cd "$prefix" && pwd)"
archive="$work/ffmpeg-9.0.2.tar.xz"
checksum=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
url=https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz
if [[ ! -f "$archive" ]]; then
  curl --fail --location --retry 2 --connect-timeout 30 --max-time 180 "$url" -o "$archive.download"
  printf '%s  %s\n' "$checksum" "$archive.download" | sha256sum --check --status
  mv "$archive.download" "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status
run_dir="$(mktemp -d "$work/build-9.0.2.XXXXXX")"
tar -xJf "$archive" -C "$run_dir"
source_dir="$run_dir/ffmpeg-9.0.2"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
real_pkg_config="$(command -v pkg-config)"
placebo_prefix="$("$real_pkg_config" --variable=prefix libplacebo)"
placebo_source="${NICONEON_LIBPLACEBO_SOURCE_DIR:-$placebo_prefix/share/niconeon/libplacebo-source}"
[[ -f "$placebo_prefix/lib/libplacebo.a" ]] || { echo "build PIC libplacebo 7.387.0 first" >&2; exit 1; }
"$real_pkg_config" --atleast-version=7.387.0 libplacebo
vulkan_archive="$placebo_source/Vulkan-Headers-v1.4.368.tar.gz"
printf '%s  %s\n' 5c5c6f64fcce28a24cc934143a048707af036a858e4ed11d7f71fe016006ecda "$vulkan_archive" | sha256sum --check --status
mkdir "$run_dir/vulkan"
tar -xzf "$vulkan_archive" --strip-components=1 -C "$run_dir/vulkan"
# Only libplacebo needs private static dependencies; other libraries stay shared.
export NICONEON_REAL_PKG_CONFIG="$real_pkg_config"
cat > "$run_dir/pkg-config" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
for argument in "$@"; do
  if [[ "$argument" == *libplacebo* ]]; then
    # libplacebo includes C++ objects (notably convert.cc). FFmpeg probes
    # this C API by linking a C executable, not a C++ application.
    # pkg-config's static closure does not imply the compiler's C++ runtime.
    for option in "$@"; do
      if [[ "$option" == --libs ]]; then
        libraries=$("$NICONEON_REAL_PKG_CONFIG" --static "$@")
        printf '%s -lstdc++\n' "$libraries"
        exit 0
      fi
    done
    exec "$NICONEON_REAL_PKG_CONFIG" --static "$@"
  fi
done
exec "$NICONEON_REAL_PKG_CONFIG" "$@"
WRAPPER
chmod +x "$run_dir/pkg-config"
# Keep dependency diagnostics visible even when configure exits before the
# corresponding-source bundle can be created.
printf 'libplacebo pkg-config executable: %s\n' "$real_pkg_config"
printf 'libplacebo version: '
"$run_dir/pkg-config" --modversion libplacebo
printf 'libplacebo static CFLAGS: '
"$run_dir/pkg-config" --cflags libplacebo
printf 'libplacebo static LIBS (including C++ runtime): '
"$run_dir/pkg-config" --libs libplacebo
"$baseline" -hide_banner -buildconf > "$run_dir/baseline-buildconf.txt" 2>&1
python3 - "$run_dir/baseline-buildconf.txt" "$run_dir/baseline-options.nul" <<'PY'
import pathlib, shlex, sys
text = pathlib.Path(sys.argv[1]).read_text()
if 'configuration:' not in text:
    raise SystemExit('cannot read baseline FFmpeg configuration')
options = shlex.split(text.split('configuration:', 1)[1])
if '--enable-gpl' not in options or '--enable-nonfree' in options:
    raise SystemExit('expected redistributable GPL-enabled Debian baseline')
removed = {'--disable-omx', '--enable-libglslang'}
owned = ('--prefix=', '--libdir=', '--incdir=', '--bindir=', '--datadir=',
         '--docdir=', '--mandir=', '--extra-version=', '--pkg-config=',
         '--extra-cflags=', '--extra-ldflags=')
options = [o for o in options if o not in removed and not o.startswith(owned)
           and o not in ('--enable-static', '--disable-shared', '--enable-shared', '--disable-static')]
if not options or any(not o.startswith('--') for o in options):
    raise SystemExit('unexpected baseline configure arguments')
pathlib.Path(sys.argv[2]).write_bytes(b'\0'.join(o.encode() for o in options) + b'\0')
PY
mapfile -d '' -t options < "$run_dir/baseline-options.nul"
# Assert the new Vulkan shader compiler path rather than silently losing filters.
shader_compiler="$(command -v glslc || command -v glslangValidator || command -v glslang)"
[[ -n "$shader_compiler" ]] || { echo "install glslc or glslang-tools" >&2; exit 1; }
cd "$source_dir"
if ./configure "${options[@]}" --prefix="$prefix" --libdir="$prefix/lib" \
  --incdir="$prefix/include" --bindir="$prefix/bin" --enable-shared --disable-static \
  --pkg-config="$run_dir/pkg-config" --glslc="$shader_compiler" \
  --extra-cflags="-I$run_dir/vulkan/include" \
  --extra-ldflags="-Wl,--exclude-libs,libplacebo.a -Wl,-rpath,$prefix/lib" \
  --extra-version=niconeon; then
  :
else
  configure_status=$?
  echo "FFmpeg configure failed (status $configure_status); config.log tail follows" >&2
  if [[ -f ffbuild/config.log ]]; then
    tail -n 240 ffbuild/config.log >&2
  else
    echo "FFmpeg did not create ffbuild/config.log" >&2
  fi
  exit "$configure_status"
fi
# configure can auto-disable Vulkan shaders without an error: fail closed.
grep -qx 'CONFIG_SCALE_VULKAN_FILTER=yes' ffbuild/config.mak || { echo "FFmpeg SPIR-V compiler unavailable" >&2; exit 1; }
make -j "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
make install
export LD_LIBRARY_PATH="$prefix/lib:${LD_LIBRARY_PATH:-}"
source_bundle="$prefix/share/niconeon/ffmpeg-source"
mkdir -p "$source_bundle/notices"
cp "$archive" "$script_dir/build_ffmpeg.sh" "$run_dir/baseline-buildconf.txt" "$source_bundle/"
cp "$run_dir/pkg-config" "$source_bundle/pkg-config-wrapper.sh"
cp ffbuild/config.log ffbuild/config.mak config.h "$source_bundle/"
cp COPYING* LICENSE.md "$source_bundle/notices/"
cp "$vulkan_archive" "$source_bundle/"
# Compare advertised feature names, not hardware availability or performance.
cat > "$source_bundle/check_features.py" <<'PY'
import json, pathlib, re, subprocess, sys
baseline, current, output = sys.argv[1:]
def features(executable, kind):
    r = subprocess.run([executable, '-hide_banner', '-' + ('pix_fmts' if kind == 'pixel_formats' else kind)], capture_output=True, text=True, check=True)
    names = set()
    if kind == 'protocols':
        direction = None
        for line in r.stdout.splitlines():
            if line in ('Input:', 'Output:'):
                direction = line[:-1]
            elif direction and re.fullmatch(r'\s+[a-zA-Z0-9_]+', line):
                names.add(direction + ':' + line.strip())
    elif kind == 'hwaccels':
        names = {l.strip() for l in r.stdout.splitlines() if re.fullmatch(r'[a-zA-Z0-9_]+', l.strip())}
    elif kind == 'pixel_formats':
        for line in r.stdout.splitlines():
            match = re.match(r'^\s*[IOHPB.]{5}\s+([a-zA-Z0-9_]+)\s+', line)
            if match:
                names.add(match[1])
    else:
        for line in r.stdout.splitlines():
            match = re.match(r'^\s+[A-Z.]{1,6}\s+([a-zA-Z0-9_,]+)\s+', line)
            if match:
                names.update(match[1].split(','))
    if not names:
        raise SystemExit('could not parse ' + kind + ' from ' + executable)
    return names
# Only confirmed removals from the pinned upstream 9.0.2 source are exempt.
sonic_source = 'https://github.com/FFmpeg/FFmpeg/commit/ca1d253621d4643b7b9ba13ded6dfc68c329fda5'
rawvideo_source = 'https://github.com/FFmpeg/FFmpeg/blob/n9.0.2/libavcodec/raw_pix_fmt_tags.h#L249-L251'
pp_source = 'https://github.com/FFmpeg/FFmpeg/commit/8c920c4c396163e3b9a0b428dd550d3c986236aa'
hls_source = 'https://github.com/FFmpeg/FFmpeg/commit/64fafd63f0b4ebf8dbbdbdc2296f21a03548b5fc'
allowed = {
    'decoders': {'sonic': sonic_source, **dict.fromkeys(('v308', 'v408', 'v410'), rawvideo_source)},
    'encoders': {'sonic': sonic_source, 'sonicls': sonic_source,
                 **dict.fromkeys(('v308', 'v408', 'v410'), rawvideo_source)},
    'filters': {'pp': pp_source},
    'protocols': {'Input:hls': hls_source},
}
# Rawvideo preserves the three FourCC formats; HLS uses its demuxer instead.
required_replacements = {'decoders': {'rawvideo'}, 'encoders': {'rawvideo'}, 'demuxers': {'hls'},
                         'pixel_formats': {'vyu444', 'uyva', 'v30xle'}}
report = {}
errors = {}
for kind in ('decoders', 'encoders', 'demuxers', 'muxers', 'filters', 'protocols', 'hwaccels', 'pixel_formats'):
    old, new = features(baseline, kind), features(current, kind)
    missing = old - new
    accepted = missing & allowed.get(kind, {}).keys()
    unexpected = missing - accepted
    replacements_missing = required_replacements.get(kind, set()) - new
    report[kind] = {
        'baseline': sorted(old), 'current': sorted(new), 'missing': sorted(missing),
        'allowed_removals': sorted(accepted), 'unexpected_missing': sorted(unexpected),
        'upstream_removal_sources': {name: allowed[kind][name] for name in sorted(accepted)},
        'missing_replacements': sorted(replacements_missing),
    }
    if unexpected or replacements_missing:
        errors[kind] = {'unexpected_missing': sorted(unexpected),
                        'missing_replacements': sorted(replacements_missing)}
pathlib.Path(output).write_text(json.dumps(report, indent=2) + '\n')
if errors:
    raise SystemExit('FFmpeg unanticipated feature loss: ' + json.dumps(errors))

PY
python3 "$source_bundle/check_features.py" "$baseline" "$prefix/bin/ffmpeg" "$source_bundle/features.json"
"$prefix/bin/ffmpeg" -hide_banner -buildconf > "$source_bundle/buildconf.txt" 2>&1
: > "$source_bundle/libraries.sha256"
for library in libavcodec.so.63 libavformat.so.63 libavdevice.so.63 libavfilter.so.12 \
  libavutil.so.61 libswresample.so.7 libswscale.so.10; do
  runtime="$prefix/lib/$library"
  [[ -f "$runtime" ]] || { echo "missing FFmpeg runtime: $runtime" >&2; exit 1; }
  symbols="$(nm -D "$runtime")"
  if grep -Eq '[[:space:]]pl_[[:alnum:]_]+' <<< "$symbols"; then
    echo "FFmpeg libplacebo symbols are not isolated: $runtime" >&2; exit 1
  fi
  printf '%s  %s\n' "$(sha256sum "$runtime" | cut -d ' ' -f 1)" "$library" >> "$source_bundle/libraries.sha256"
done
cat > "$source_bundle/README.txt" <<MANIFEST
FFmpeg 9.0.2, unmodified upstream source, GPL-enabled Debian feature baseline.
Source: $url
Source SHA-256: $checksum
Build: bash build_ffmpeg.sh /absolute/prefix
Install Debian 13 ffmpeg build dependencies using the matching deb-src suite,
plus glslc, and build the supplied libplacebo 7.387.0 PIC static library first.
Baseline build configuration and exact resulting config/logs are included.
Removed upstream options: --disable-omx; --enable-libglslang is replaced by the
build-time SPIR-V compiler. Feature inventories detect unanticipated feature loss.
Explicit upstream removals: Sonic decoder and sonic/sonicls encoders, pp filter,
and the obsolete Input:hls protocol. Sonic and pp behavior is genuinely removed;
this is not full feature equivalence. The HLS demuxer remains required.
v308/v408/v410 codec names are replaced by rawvideo with vyu444/uyva/v30xle.
The pinned source raw_pix_fmt_tags.h lines 249-251 maps all three FourCCs;
rawvideo encoder/decoder and all three pixel formats are required. features.json separately records actual
missing names, allowed removals, exact upstream evidence, and unexpected loss.
No arbitrary feature loss is accepted merely because a report labels it allowed.
Feature inventory is not hardware-accelerator or media-quality validation.
libplacebo is statically linked with hidden symbols. Its separate corresponding
source bundle (libplacebo-source) is required alongside this bundle.
Vulkan-Headers v1.4.368 is included and checksum verified by the build script.
libavcodec/libavformat/libavdevice 63.1.102, libavfilter 12.1.102,
libavutil 61.1.102, libswresample 7.1.102, libswscale 10.1.102.
libraries.sha256 identifies the seven installed shared runtimes before packaging.
Other libraries remain distribution-provided; their source/license information
must accompany distribution under the repository's native-license process.
MANIFEST
printf 'Installed FFmpeg 9.0.2 with checked baseline features into %s\n' "$prefix"
