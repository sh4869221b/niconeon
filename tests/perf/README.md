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
composited over the captured video background. It requires nonempty reference
ink and no channel error above 8 over the inspected image. Captures and failed
reference images are saved beside JSON. DPI 1/1.5/2 and atlas/frame_image should be
run separately with unchanged font packages. The default mode does not prove
every high-density comment's pixels independently; exact submission accounting,
existing raster pixel-equality tests, and pressure/interaction tests cover
additional, explicitly distinct claims.

The dedicated harness never establishes normal-QoS behavior, real hardware-GPU
performance, Windows/Wayland/HDR correctness, 60fps qualification or long-duration
stability. Those gates require their own evidence.
