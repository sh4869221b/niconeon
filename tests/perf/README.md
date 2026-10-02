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

The raster summary also retains completion-wake pending/active counts, notification
and coalescing totals, pending/outstanding high-water marks, maximum committed
sprites per wake, and no-progress callbacks. The explicit bound check is pending
<=1, active<=1, outstanding high-water<=2, and at most 8 committed sprites per wake;
it is not a claim of GPU completion or total render-work capacity.

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

# Fixed finite workload above the old allocator limit: all admitted IDs must draw.
./build/release/real_render_profile --sample-mode pixels --pixel-suite active-capacity \
  --renderer atlas --expected-dpr 1 --output /tmp/active-capacity.json
```

`wide` inspects both ends of a >2048-logical-pixel sprite, every internal 2046px core-tile
boundary centered in the viewport (including neighboring gutter pixels),
and a fractional x=4.25 position. The source boundary is calculated from an
independent reference alpha bounding box plus the one-pixel margin, using the
original full-image-to-logical-size scale, and its visibility is recorded and
required. It uses the controller's normal drag API.
Fractional probes use the CPU reference's linear image interpolation, matching
the renderer's linear sampler; channel tolerance remains 8.

`atlas-pressure` adapts to cropped sprite dimensions rather than assuming the
old 42px full-image packing height. Source images still have measured physical
widths >1024 and <=2048. Actual submission page masks select one persistent anchor
for each of the eight atlas pages. Small, nonoverlapping batches keep all known
anchors active and add fresh texts, up to a hard bound of 4096 distinct strings.
Coverage requires all eight pages to contain active anchors and an observed
repack that preserves active sprites. Every batch receives the same strict pixel
comparison. A bounded replay then searches the whole historical non-anchor
cohort until another active-preserving repack occurs; it does not assume that a
particular page or the first cohort was evicted. Missing page metadata, unmet
coverage, observer overflow or pixel mismatch fails explicitly.

This corrects an observed fixture assumption: the batch planner reclaimed the
largest inactive page and could leave the original sentinel's page untouched.
In that run every pressure capture matched, while first-page-only replay never
repacked. Passing pixels did not establish the intended eviction/recovery coverage.

`active-capacity` retains exactly 385 active inputs at DPR1 and 193 at DPR2 for
before/after comparison. These exceed the old uncropped shelf allocator's limit;
they are not asserted to exceed the current cropped/tiled allocator's capacity.
The `finite-active-legacy-limit` result requires current-frame logical draw-ID
completeness and zero remaining missing/unresident sprites. Overlapping pixels
are saved for diagnosis but cannot qualify individual readability. Tile quad
counts and logical submitted-comment counts are recorded separately.

`appearance` uses white text and color emoji to compare fractional positioning,
NG hover tint, intermediate fade and disappearance against the same gray video.
The intermediate fade is driven through the public NG API; its observed snapshot
alpha must be between 0.25 and 0.75 and is recorded. The oracle compares against
that exact alpha, not a claimed deterministic 0.5 injection. Fade completion must
restore the clean video background; it tests removal, not a direct alpha-zero
shader injection. Tint/fade coverage requires the same 120 ink pixels in the
untinted full-opacity source reference, since tinted or translucent output is
intentionally not white. Output pixels still use the unmodified error limit 8.

```sh
./build/release/real_render_profile --sample-mode pixels --pixel-suite appearance \
  --renderer atlas --expected-dpr 1 --output /tmp/appearance-atlas.json
./build/release/real_render_profile --sample-mode pixels --pixel-suite appearance \
  --renderer frame_image --expected-dpr 1 --output /tmp/appearance-frame-image.json
```

`all` runs every suite. Each suite in a separate process is preferable for failure
isolation and independent instrumentation bounds. A blocked or missed
intermediate-fade observation is failed evidence; the test never substitutes an
opaque or absent frame as a passing intermediate sample.

For DPR2, provide a sufficiently large real/Xvfb display (for example 2560x1600)
and use `--expected-dpr 2`; capture dimensions are checked against the actual
window DPR. The same suite can be run at DPR1.5. Run basic/wide/appearance on `frame_image`
separately; atlas-pressure and active-capacity require the atlas backend.
An unsupported oversized sprite, missing glyph, observed corruption, or unmet
coverage prerequisite remains failed evidence. Never weaken the pixel oracle or
silently skip such a case. These tests do not prove every high-density workload's
pixels independently; selected suite, DPR, fonts and backend bound each claim.

The dedicated harness never establishes normal-QoS behavior, real hardware-GPU
performance, Windows/Wayland/HDR correctness, 60fps qualification or long-duration
stability. Those gates require their own evidence.

The CI discovery matrix now retains the original 400 cps case and adds 100/200 cps
with otherwise identical text, fixture, 30s feed, 15s drain and two BA/AB pairs.
These are not qualification runs: only cases where both arms complete the same
work may advance to the predeclared >=10 paired / >=1000 feed-frame analysis.
A lower-rate result never establishes that the retained 400 cps case passes.
Fractional-position probes use an independent full-image bilinear oracle because
QPainter's equal-scale translate fast path snaps even with SmoothPixmapTransform.
Integer positions retain the original QPainter reference. Destination physical
pixel center `(x+0.5)/targetDPR` minus logical translation, times sourceDPR,
minus0.5 gives the source texel-center coordinate. Four clamped neighbors are
interpolated in premultiplied RGBA and source-over blended. No atlas crop, UV,
tile or candidate output is used to choose this coordinate convention.
The unit oracle covers physical +/-0.25/0.5 offsets, DPR1/1.5/2 and three font
families, checks integer equivalence, and rejects snapped / one-pixel-shifted
negative controls at the unchanged channel threshold8. Actual GL probes retain
wide internal seams and add positive/negative fractional positions. No threshold
is relaxed after a failure. Texture recreation must retain Linear/ClampToEdge.

## Prespecified 100 cps study (2026-10-02)

This bounded study is separate from the retained 100/200/400 discovery job and does
not establish 400cps acceptance. It compares final-ID-complete timing while explicitly
retaining the baseline's existing transient missing-image quality failure. It never
exempts candidate quality errors, terminal missing IDs, expiry, backlog, other error
messages, unexpected process failures, metadata/hash mismatch, or observer overflow.
The analyzer's default remains strict; the study selects `--baseline-transient-missing`
and records that policy in its output. Raw `success=false`, errors, counts, process exit
and the manifest remain unchanged. A baseline can therefore fail quality while its
complete-work timing is eligible for this narrowly scoped comparison.

The plan is fixed before measurement:

- Baseline3141e66 plus observer-only patch, identical harness, worker on, atlas on
- 100 unique comments/sec for180s plus15s fixed drain:18,000 comments/run
- Identical deterministic240s H.264 fixture within the study,1280x720,DPR1, same fonts
- One GitHub runner/container per study, Mesa software with2render threads; build both
  arms before measurement and run them sequentially without concurrent compilation
- Normal video/glyph startup warmup first: wait until video position>=500ms, then pause
  and seek to exact media zero. No comment feed during warmup. Clear workload counters
  and start the measured phases only afterward. No post-hoc warmup/sample deletion
- Ten adjacent pairs, randomized balanced5AB/5BA, runner seed20261002,1s between processes
- Every frame and heartbeat run must contain>=1000feed intervals; otherwise inconclusive
- Nearest-rank p95/p99 in feed and full windows: all8frame/heartbeat metrics required
- Paired log-ratio bootstrap resamples whole pairs, never individual frames,10,000
  iterations, seed20261002. Each pointwise two-sided95%CI upper ratio must be<=1.0
  (zero permitted regression). n=10and pointwise rather than family-wise CI limitations
  remain explicit; this is within-host evidence, not a hardware-wide guarantee
- First-draw lateness, total logical-instance draw count and frame-held instance-seconds
  are also reported. The latter includes clipped/overlapping text and is only a workload
  proxy, not exact visible-pixel duration. Final ID equality is not identical pixel work
- All planned runs, including aborts/timeouts/failures, remain in the evidence. No selective
  retries or changing margins/sample thresholds after seeing results
- Approximately65minutes of timing,80minute job limit. Existing discovery/pixel job runs
  separately with its40minute limit. An incomplete job is not an acceptance result

The harness feed-duration bound is180s solely to collect enough low-FPS samples;
its60,000-comment trace bound and every production queue/resource bound are unchanged.

## Optional GL command-interval probe

`NICONEON_RENDER_GPU_TIMING=1` additionally enables an opt-in timestamp probe when
render diagnostics are enabled. It uses eight pairs (sixteen query objects), reads
at most one old pair per render, and checks both results' availability before
retrieval. A full ring skips a new sample instead of blocking or overwriting it.
There is no glFinish, retry loop, queued callback or GUI GL access. OpenGL3.3 or
ARB_timer_query is required; ES/unsupported contexts explicitly report unavailable.

`gpu_elapsed_ns` belongs to `gpu_measured_frame_sequence` and
`gpu_measured_cpu_start_elapsed_ns`, not the row in which that delayed result was
collected. `gpu_result_available`, support, original draw-call count, pending,
skipped and invalid-result fields are mandatory interpretation context. The final
up-to-eight pending queries may remain unmeasured at shutdown/resource reset.
The scope is the overlay's GL command interval, including texture work and possible
command-producer gaps. It is not physical presentation, GPU utilization, or real
hardware qualification when running software Mesa. Logging/JSON occurs outside
the bracket. QSGRenderNode's documented current-context release/destructor contract
owns deletion, and release supports a later clean reinitialization.

The normal acceptance study leaves this flag off. The analyzer treats timestamp-
instrumented runs as diagnostic-only, even when ID/quality prerequisites hold.
The same opt-in observer is applied to the baseline without renderer optimizations.

References: [Qt timer query](https://doc.qt.io/qt-6/qopengltimerquery.html),
[QSGRenderNode resource lifecycle](https://doc.qt.io/qt-6/qsgrendernode.html#releaseResources).

The `gpu400` CI study is diagnostic-only: two balanced BA/AB pairs at400unique
comments/sec,30s feed+15s drain, seed20261002, with the timestamp flag enabled
for both arms. The independent report requires at least100completed drawing
intervals/run, no skipped/invalid queries, and the eight-pair bound. It retains
all raw quality failures and does not grant frame-time or complete-text acceptance.
PR mode selection is documented below; `workflow_dispatch` selects discovery,
gpu400, cpu400 or formal100 explicitly. The latter remains an uninstrumented65minute study.

### Prespecified100cps result (2026-10-02)

Immutable plan/harness/analyzer: commit `d79fb939ead68672a66313352a11a044979c3e42`.
[Exact study run](https://github.com/sh4869221b/niconeon/actions/runs/36948408144),
[raw20-run artifact](https://github.com/sh4869221b/niconeon/actions/runs/36948408144/artifacts/11204794309).
The ZIP is22,187,036bytes, SHA256
`bd0e6dbe23f7cb4305cf19409de0b35c696c796feb57761e79dc5cab1d40ef50`.
The complete manifest and analyzer hashes are embedded in the artifact. No planned
run was omitted or retried. All20runs reached18,000actual first-draw IDs, zero
pending/failed/expired at the end. Candidate missing-image observations were zero;
the baseline retains its raw quality failure with383,164–408,926transient missing
image observations/run. This is a complete-ID comparison, not identical pixel work.

| Metric | Candidate change | Pointwise paired95%CI |
|---|---:|---:|
| Feed frame p95 | -29.900% | [-30.249,-29.551]% |
| Feed frame p99 | -29.069% | [-30.035,-28.164]% |
| Feed heartbeat p95 | -30.000% | [-30.293,-29.681]% |
| Feed heartbeat p99 | -29.723% | [-30.561,-28.999]% |
| Full frame p95 | -29.673% | [-30.015,-29.340]% |
| Full frame p99 | -29.457% | [-30.225,-28.765]% |
| Full heartbeat p95 | -29.533% | [-29.854,-29.189]% |
| Full heartbeat p99 | -29.829% | [-30.581,-29.188]% |

All eight predeclared noninferiority gates pass within this software host study.
Candidate feed framep99 was110.552–120.080ms versus160.064–166.273ms baseline; this is not
60fps qualification. Source-to-first-drawp99 was0.546–0.559s versus4.978–5.301s.
Candidate draw-frame instances1.910–1.931million versus1.024–1.045million;
frame-held instance-seconds192,828–192,936versus136,673–139,880. Those are logical
work proxies, include clipped/overlapping text, and show why final ID equality
must not be presented as identical visible-pixel exposure.

All ten candidate GL pixel suites/DPR combinations passed the unchanged threshold;
baseline emoji corruption remains a failed negative control. The separate400cps
n=2discovery remains incomplete (candidate11,740/11,664of12,000drawn;219/281expired;
41/55activated but not drawn). Issue65is therefore still open and the PR stays Draft.
Neither this study nor the timestamp probe qualifies physical GPU/Wayland/HDR hardware.

### Timestamp reliability and CPU diagnostic boundary

The d835095400cps probe retained all four runs but its diagnostic gate failed.
[Raw artifact](https://github.com/sh4869221b/niconeon/actions/runs/36955826098/artifacts/11206455464)
SHA256 `debbd814b3ff3f229b8f4ecece067e08e50cf06fc74ba003039e817337ef50f3`.
A render snapshot/sync serial is not a unique render invocation: Qt may render the
same snapshot repeatedly. Query identity is its original render-start timestamp
plus sync serial, and per-render query outputs must clear even without a new sync.
This correction does not make the measured Mesa intervals qualified GPU timing.

Mesa25.0.7 llvmpipe returned mostly30ns intervals despite expensive frames. Its
[source query handler](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-25.0.7/src/gallium/drivers/llvmpipe/lp_rast.c#L657)
records end markers per raster bin and overwrites a per-thread timestamp. Such
measurements need an independent whole-work control; do not infer a cheap overlay
or justify an optimization from these results. The report flags llvmpipe intervals
as unqualified, retains all data, and never grants GPU-time acceptance.

The existing CPU phase trace locates candidate feed p99 at about121ms between
Qt afterRendering and frameSwapped, versus renderer CPU calls around4ms and
sync around6ms. These separate percentiles are not additive or per-frame causal
proof. libmpv render p99 is about32ms. Candidate first-draw counts were11,669/11,641
of12,000, with272/309preactivation expirations;400cps remains incomplete.

`cpu400` is one bounded diagnostic step, not another acceptance trial:

- Independent `gl_timestamp_control`:64/256/768px framebuffer, full/quarter scissor,
 128known blended draws,3trials per condition, query interval plus CPU wall time
 to explicitly completed work and pixel readback. This isolated executable uses
 glFinish intentionally, never in the app or timing study;45second safety timeout
- One candidate400cps30s feed+15s drain CPU sample profile, timestamp flag off,
 official Debian gperftools `libprofiler`,100Hz SIGPROF,110second process timeout
- Profiling includes process startup and may perturb scheduling. Stripped library
 and JIT symbols can limit attribution. Neither sampled CPU data nor its frame
 distribution is used for performance acceptance; raw quality failures stay visible
- No perf/sysctl/security changes or credentials. Reports and the binary profile
 stay in the diagnostic artifact; no profiler dependency is added to production
- After this control/profile, either use an identified hot path to justify a bounded
 change with strict pixel regression tests, or record a hardware-measurement blocker.
 No further timestamp-only iteration is a substitute for an actionable cause

PR pushes now select discovery+cpu400; gpu400/formal100 remain explicit modes.
The pixel mapping/seam suites additionally cover DPR1.5 without changing the
historical finite-pressure workloads atDPR1/2 or the error threshold.
