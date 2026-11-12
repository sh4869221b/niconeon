#!/usr/bin/env bash
# Verify pristine installed runtimes before packaging tools rewrite ELF paths.
set -euo pipefail
[[ $# -eq 2 ]] || { echo "usage: $0 <license-directory> <media-prefix>" >&2; exit 2; }
source_dir="${NICONEON_FFMPEG_SOURCE_DIR:-$2/share/niconeon/ffmpeg-source}"
for file in ffmpeg-9.0.2.tar.xz build_ffmpeg.sh README.txt libraries.sha256 \
  baseline-buildconf.txt buildconf.txt features.json check_features.py \
  config.log config.mak config.h pkg-config-wrapper.sh notices/LICENSE.md \
  notices/COPYING.GPLv2 notices/COPYING.LGPLv2.1 Vulkan-Headers-v1.4.368.tar.gz; do
  [[ -f "$source_dir/$file" ]] || { echo "missing supported FFmpeg corresponding source: $file" >&2; exit 1; }
done
printf '%s  %s\n' 8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e "$source_dir/ffmpeg-9.0.2.tar.xz" | sha256sum --check --status
printf '%s  %s\n' 5c5c6f64fcce28a24cc934143a048707af036a858e4ed11d7f71fe016006ecda "$source_dir/Vulkan-Headers-v1.4.368.tar.gz" | sha256sum --check --status
python3 - "$source_dir/libraries.sha256" "$2/lib" "$source_dir/features.json" <<'PY'
import hashlib, json, pathlib, re, sys
manifest, runtime_dir, features = sys.argv[1:]
expected = {'libavcodec.so.63', 'libavformat.so.63', 'libavdevice.so.63',
            'libavfilter.so.12', 'libavutil.so.61', 'libswresample.so.7', 'libswscale.so.10'}
seen = set()
for line in pathlib.Path(manifest).read_text().splitlines():
    m = re.fullmatch(r'([a-f0-9]{64})  ([a-z0-9.]+)', line)
    if not m or m[2] not in expected or m[2] in seen:
        raise SystemExit('invalid FFmpeg runtime manifest')
    path = pathlib.Path(runtime_dir) / m[2]
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != m[1]:
        raise SystemExit('FFmpeg runtime does not match source bundle: ' + str(path))
    seen.add(m[2])
if seen != expected:
    raise SystemExit('incomplete FFmpeg runtime manifest')
report = json.loads(pathlib.Path(features).read_text())
if set(report) != {'decoders', 'encoders', 'demuxers', 'muxers', 'filters', 'protocols', 'hwaccels', 'pixel_formats'}:
    raise SystemExit('incomplete FFmpeg feature verification')
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
for kind, item in report.items():
    old, new = set(item['baseline']), set(item['current'])
    missing = old - new
    accepted = missing & allowed.get(kind, {}).keys()
    unexpected = missing - accepted
    replacements_missing = required_replacements.get(kind, set()) - new
    sources = {name: allowed[kind][name] for name in sorted(accepted)}
    if (not old or unexpected or replacements_missing
            or item['missing'] != sorted(missing)
            or item['allowed_removals'] != sorted(accepted)
            or item['unexpected_missing'] != sorted(unexpected)
            or item['upstream_removal_sources'] != sources
            or item['missing_replacements'] != sorted(replacements_missing)):
        raise SystemExit('invalid FFmpeg feature verification or unanticipated loss: ' + kind)

PY
mkdir -p "$1/ffmpeg-source"
cp -R "$source_dir/." "$1/ffmpeg-source/"
