# Raster performance evidence

`raster_profile` is a CPU-only scheduling microbenchmark. It does not run the
video renderer, upload textures, observe presentation, or exercise application QoS.
It cannot establish #65's real-render performance acceptance on its own.

`real_render_profile` runs the production `MpvItem`, `DanmakuController`, and
`DanmakuRenderNodeItem` on an actual Qt Quick OpenGL window. It preserves the
production glyph-warmup Text item but deliberately excludes unrelated application
controls and the adaptive QoS controller. Normal application/QoS measurements
remain a separate required experiment.

## Build and identical-source comparison

```sh
cmake --preset release -DNICONEON_BUILD_PERF_TOOLS=ON
cmake --build --preset release --target real_render_profile
```

Use the same harness, renderer observer, dependencies, build type, compiler flags,
and CMake target in both baseline and candidate builds. The harness adapts to the
baseline controller's void admission API. Both builds need the *same opt-in
renderer diagnostics patch*. Do not backport the optimization to the baseline.
Record both Git revisions, the observer patch SHA-256, executable hashes, toolchain
and runtime package versions. A dirty candidate must have its complete diff saved.

A real OpenGL display is required. Where authorized and available, a reproducible
software path is Xvfb plus Mesa llvmpipe; it is not hardware-GPU qualification.
A blocked/missing display or skipped pixel test is unverified, never a pass.

## One fixed video fixture

Generate once, retain the file, record its SHA-256, and reuse the exact bytes in
both arms. For example:

```sh
ffmpeg -f lavfi -i 'testsrc2=size=640x360:rate=30' -t 180 \
  -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p -an \
  perf-sm9-640x360-30fps-180s.mp4
sha256sum perf-sm9-640x360-30fps-180s.mp4
```

The recipe alone does not guarantee byte-identical output across ffmpeg versions.
The harness checks media duration before timing. It records the actual video hash,
Qt/mpv versions, GL renderer/vendor/version, font and DPR. It uses DejaVu Sans
with platform fallback, a 1280x720 viewport, 24px raster text, 36px lane spacing
plus a 6px gap, playback rate 1, 60fps target, and QT_HASH_SEED=0. The hash seed is
essential because comment IDs determine motion speed through qHash.

Keep SIMD mode, scaling options, display/backend, CPU affinity, power state and
other activity fixed. Do not reduce video interpolation quality, viewport size,
text complexity, comment visibility, or input rate in one arm to improve timing.

## Timing and completion

```sh
./build/release/real_render_profile \
  --video perf-sm9-640x360-30fps-180s.mp4 --output /tmp/run.json \
  --cps 400 --duration-ms 30000 --tail-ms 15000 \
  --text-mode unique --sample-mode timing --worker on --renderer atlas
```

The deterministic mixed corpus covers Japanese, combining marks, Arabic and emoji.
`unique` gives each comment distinct text; `warm` cycles 32 complete strings while
keeping comment IDs unique. A 50ms source timer and 16ms bounded admission drain
retain two source batches and retry unaccepted prefixes, with no source dropping
or adaptive QoS. After initial video/GL startup the harness pauses and confirms an exact seek to
media zero, so both arms replay the same video interval. The feed phase follows
actual media position; a fixed drain tail
is reported separately and never dilutes feed-phase percentiles. Full fixed-window
percentiles are also reported so work deferred into the tail cannot disappear
from the comparison. Feed-end admission/submission counts and first-submission
latency expose reduced timely throughput even when final total work is equal.

The default 30s feed produces exactly 3,000 / 6,000 / 12,000 comments at 100 / 200 /
400 cps. Source backpressure does not reduce this number. Failure to admit or
submit every expected ID before the common fixed end, an unexpected ID, raster
failure, expiry, missing image, unresident sprite, or observer overflow fails the
strict completeness/quality prerequisite. Full raw evidence remains available.
Transient baseline missing-image observations are not silently conflated with
final never-submitted IDs or dropped input; they are retained separately.

Raw frame intervals are captured with a monotonic clock in `frameSwapped`'s direct
render-thread callback. The 16ms GUI heartbeat is a distinct metric. `frameSwapped`
is evidence of Qt's swap notification, not physical monitor scanout or GPU
completion. Renderer submission events mean that an ID reached an actual draw
call, not that its pixels were individually readable on screen. The separate
pixel oracle is mandatory corroborating correctness evidence.

During timing there is no pixel readback, JSON serialization, or file output from
the profiler. Bounded renderer structs and timestamp arrays are serialized after
measurement. Renderer timing fields overlap (`setFrameNs` includes its substeps),
so do not sum nested durations. Multiple render callbacks can share a
`frameSequence`; do not deduplicate by that field. GL upload bytes are distinct
from received sprite bytes and must not be substituted for each other.

## Paired campaigns and analysis

```sh
python3 tests/perf/run_real_render_comparison.py \
  --baseline /path/base/real_render_profile \
  --candidate /path/candidate/real_render_profile \
  --video perf-sm9-640x360-30fps-180s.mp4 \
  --output-dir /tmp/comparison --pairs 10 --keep-going
python3 tests/perf/analyze_real_render.py /tmp/comparison \
  --output /tmp/comparison/analysis.json
```

The runner randomizes case order within each replicate and balances adjacent AB/BA
pairs. It launches fresh processes with isolated XDG settings/data/cache and
records commands, allow-listed environment, stdout/stderr, executable/video/raw
hashes, and the planned run order. Failed runs cannot be silently omitted.

The analyzer uses nearest-rank p95/p99 of raw *feed-phase* and full fixed-window
intervals per run as separate metrics.
It never averages two-second window percentiles. It bootstraps complete paired
runs, not autocorrelated individual frames, to report 95% confidence intervals on
candidate/baseline geometric-mean ratios. The default requires ten pairs and
at least 1,000 samples per feed stream per run. If overload leaves too few frames,
extend the same media/feed duration in both arms in a predeclared new campaign;
do not pool independently timed windows or treat an inconclusive result as pass.

Zero regression tolerance is the default. Any practical noninferiority margin
must be explicitly selected and disclosed before a qualification campaign, never
chosen after looking at the result. A two-pair smoke run can expose gross
regressions or broken accounting but does not establish tail-latency acceptance.
All declared workloads and both p95/p99 streams must qualify; a favorable warm
case cannot cancel an unfavorable high-uniqueness case. CIs are pointwise, not
simultaneous family-wise bands; machine/driver scope and remaining uncertainty
must accompany the result.

## Separate pixel oracle

```sh
./build/release/real_render_profile --sample-mode pixels \
  --output /tmp/pixels-atlas.json --renderer atlas --worker on
./build/release/real_render_profile --sample-mode pixels \
  --output /tmp/pixels-frame-image.json --renderer frame_image --worker on
```

This untimed mode creates a deterministic gray Y4M video, proves video pixels are
present and uncorrupted, pauses playback, and compares separately rendered
Japanese/combining/Arabic/emoji/Latin samples with synchronous reference sprites
composited over the captured video background. Channel tolerance stays 8, and
every visible instance must contain at least 120 reference ink pixels. Exact
expected IDs and unchanged positions/widths are checked before and after capture;
nonoverlapping probe rectangles prevent one text from hiding another's omission.
A render callback after the expected state change and actual draw submissions
must be observed. Captures and failed reference images are saved beside JSON.

Full-string QTextLayout shaping inspects all fallback QGlyphRun objects. Empty
glyph output, invalid fallback fonts, and glyph index 0 fail independently of
pixel equality, so a shared missing-glyph result cannot pass merely because both
paths draw the same tofu. Fallback family names are recorded. This check does not
independently certify typography, Arabic shaping, emoji ZWJ ligatures or color;
no glyph-count/codepoint-count equality is assumed. A color-emoji discrepancy
between the CPU reference and actual renderer remains a failure.

Additional suites are deliberately separate from the default `basic` suite:

```sh
# Known wide-sprite limit: logical width exceeds 2048. No cropping/downscaling
# of the source sprite is permitted to make it fit the atlas.
./build/release/real_render_profile --sample-mode pixels --pixel-suite wide \
  --renderer atlas --expected-dpr 1 --output /tmp/wide-atlas.json

# Keep the same node/atlas alive while filling pages and evicting old residents.
./build/release/real_render_profile --sample-mode pixels --pixel-suite atlas-pressure \
  --renderer atlas --expected-dpr 1 --output /tmp/pressure-atlas.json

# A separate process must establish actual DPR2, not only a controller setting.
QT_SCALE_FACTOR=2 ./build/release/real_render_profile --sample-mode pixels \
  --pixel-suite atlas-pressure --renderer atlas --expected-dpr 2 \
  --output /tmp/pressure-dpr2.json

# Distinct overload diagnostic: do all valid admitted IDs reach a draw call?
./build/release/real_render_profile --sample-mode pixels --pixel-suite active-capacity \
  --renderer atlas --expected-dpr 1 --output /tmp/active-capacity.json
```

`wide` inspects both ends of a >2048-logical-pixel sprite, positioning it through
the controller's normal drag API. `atlas-pressure` uses measured physical widths
>1024 and <=2048, so only one sprite fits each 42*DPR-high shelf. Eight 2048-square
pages hold at most 384 such sprites at DPR1 or 192 at DPR2. Small batches preserve
an active first-loaded sentinel while prior sprites become inactive. Every batch
gets a strict pixel comparison. The suite exceeds that capacity, requires real
page allocation/repacking evidence, and replays the original first-page cohort
with fresh comment IDs, requiring additional repacking and pixel equality.
A single arbitrary old probe would not establish that evicted content was tested.

`active-capacity` keeps capacity+1 distinct sprites active simultaneously. It
reports actual draw-ID completeness and remaining missing/unresident sprites;
overlapping pixels are saved for diagnosis but cannot qualify individual
readability. This overload result is separate from the normal bounded-active
pressure/pixel suite. `all` runs every suite, including this overload diagnostic,
and can therefore expose several independent failures in one JSON file.

For DPR2, provide a sufficiently large real/Xvfb display (for example 2560x1600)
and use `--expected-dpr 2`; capture dimensions are checked against the actual
window DPR. The same suite can be run at DPR1.5. Run basic/wide on `frame_image`
separately; atlas-pressure and active-capacity require the atlas backend.
An unsupported oversized sprite, missing glyph, observed corruption, or unmet
coverage prerequisite remains failed evidence. Never weaken the pixel oracle or
silently skip such a case. These tests do not prove every high-density workload's
pixels independently; selected suite, DPR, fonts and backend bound each claim.

The dedicated harness never establishes normal-QoS behavior, real hardware-GPU
performance, Windows/Wayland/HDR correctness, 60fps qualification or long-duration
stability. Those gates require their own evidence.
