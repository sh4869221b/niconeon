# #65 discovery measurement, 2026-10-01

This is **failed acceptance evidence**, not a speedup result. Raw data is in the
[CI artifact](https://github.com/sh4869221b/niconeon/actions/runs/36931993728/artifacts/11196283115)
from head `24c1a9c55d9b3b9bedd4c14b0a1aadf6044c167a`. Baseline is `3141e66`
with the checked-in observer-only patch and identical harness source.

## Fixed setup and workload

- Debian 13 / Qt 6.8.2 / patched mpv 0.41.0 / GCC 14; Release
- Mesa 25.0.7 llvmpipe, two rendering threads; GitHub runner, four logical CPUs
- Xvfb, 1280×720 application viewport, DPR1, DejaVu Sans + Noto CJK/Color Emoji
- Same 640×360/30fps H.264 fixture, SHA-256
  `fd4bba6307ab4107be41c6927af6676b3b9ab5082a9b7a9d05ff1f74e25a078b`
- 400 unique comments/sec, 30s feed + 15s fixed drain; 12,000 offered comments
- Two paired BA/AB trials; `QT_HASH_SEED=0`; no simultaneous compilation
- Dedicated harness retains playback and glyph warmup, disables adaptive QoS;
  ordinary application/QoS was measured separately without policy changes

## Completeness and frame intervals

| Arm / trial | Accepted | Raster images received | First GL draw IDs | Pre-activation expiry | Feed p95 / p99 (ms) |
|---|---:|---:|---:|---:|---:|
| candidate 1 | 12,000 | 12,000 | 3,857 | 8,083 | 137.134 / 144.231 |
| baseline 1 | 12,000 | 8,800 | 3,184 | unavailable | 132.108 / 140.843 |
| baseline 2 | 12,000 | 8,840 | 3,265 | unavailable | 129.946 / 139.421 |
| candidate 2 | 12,000 | 12,000 | 3,882 | 8,055 | 135.513 / 144.747 |

These are nearest-rank percentiles of raw `frameSwapped` intervals, not medians
of window percentiles and not physical scanout measurements. All four runs fail
completeness. Baseline exposed more than two million cumulative missing-image
observations; these are repeated frame observations, **not distinct missing IDs**.
Candidate had zero missing-image observations but severe expiry. Another 60/63
candidate comments are not explained by first-draw plus pre-activation-expiry
counts alone. The following instrumentation separates source lateness,
post-admission waits, activation, and first draw instead of attributing them by
assumption. First draw still does not prove individually visible pixels.

## Measured renderer work

Candidate trials performed 1,464 / 1,461 page repacks and cleared approximately
23.0 GiB / 23.0 GiB of page pixels, copying 21.8 GiB / 21.8 GiB of sprites.
CPU repack calls totalled 2.82s / 2.93s; per-sync repack p99 was 23.54ms / 21.25ms.
Actual GL page transfers were 8.84 GiB / 8.69 GiB, but `setData` CPU calls totalled
only 0.528s / 0.533s. These nested CPU timings cannot explain the complete 140ms
frame tail or establish GPU elapsed time. Qt Quick phase, mpv render/property
poll, and per-comment timing observers were therefore added for the next trial.

The first targeted optimization batches pending sprites into at most one
transactional repack per page per sync. It does not enlarge the eight-page GPU
atlas or queues. It needs one additional temporary 16 MiB CPU page and evicts
inactive residency from successfully rebuilt pages, retaining CPU images.
This resource/reuse tradeoff is explicit; runtime improvement remains to be measured.

## Normal QoS and pixel correctness

Normal application trials offered 12,000 comments. Candidate admitted/drew
4,265 / 4,291 and intentionally dropped 7,735 / 7,709 through existing adaptive
QoS. Baseline admitted 11,676 / 11,635, intentionally dropped 324 / 365, but only
2,714 / 2,768 IDs reached a draw. These unequal workloads cannot certify a gain.
Candidate also had transient unresident sprites in this scenario despite every
admitted ID eventually reaching a draw.

The independent pixel oracle matched Japanese, combining/Arabic and Latin text
exactly in both arms. Color emoji failed in both: 417 pixels differed, maximum
channel error 241. The captured actual image is a white silhouette; the reference
contains the emoji colors. The inherited atlas shader sampled alpha instead of
preserving RGB. The candidate fixes ordinary premultiplied RGB while preserving
NG pink-mask tinting and applying fade to both color and alpha.

Local AF_UNIX/display restrictions prevent actual GL validation. The regular CI
for this exact head passed all five jobs, including Debug/Release/ASan+UBSan+LeakSanitizer,
strict default/bilinear OpenGL tests and Windows packaging/startup. Those existing
tests did not cover these new performance/pixel failures. No suppression, relaxed
pixel tolerance, reduced offered rate, or omitted failed run is used as acceptance.

## Second discovery head: `7acdc00`

[Regular CI](https://github.com/sh4869221b/niconeon/actions/runs/36935958567)
passed all five jobs. [Performance/pixel CI](https://github.com/sh4869221b/niconeon/actions/runs/36935958576)
**failed**, with the same 400 cps/30s + 15s drain protocol and two BA/AB pairs.
Raw [timing](https://github.com/sh4869221b/niconeon/actions/runs/36935958576/artifacts/11198333567)
and [pixel](https://github.com/sh4869221b/niconeon/actions/runs/36935958576/artifacts/11197908367)
artifacts have 14-day retention. Selected raw frame intervals, terminal summaries,
pixel outcomes and raw-file SHA-256 identifiers are retained in
[`evidence-7acdc00/results.json`](evidence-7acdc00/results.json); phase/latency
summaries are retained in [`phase-summary.txt`](evidence-7acdc00/phase-summary.txt).
Those selected records are not a replacement for all raw event traces or PNGs.

- Batch repacking reduced candidate `setFrame` p99 to 2.455 / 2.367 ms versus
  baseline 20.927 / 18.692 ms; total candidate repack CPU time was about 0.01s
  versus baseline 1.129 / 1.080s. This is a measured stage improvement only.
- Whole-frame feed p99 remained 156.737 / 157.252 ms candidate versus
  157.150 / 156.367 ms baseline. Candidate accepted only 11,216 / 11,280 of
  12,000 offered, expired 7,781 / 7,890, and submitted 3,280 / 3,214 IDs.
  Baseline submitted 2,725 / 2,743 IDs. Equal-work prerequisites still fail.
- Candidate text painting totalled about 2.9s, but admission-to-GUI-ready p99
  was about 1.23s, with the 128-request / 32-completion queues full. Frame-only
  drains imposed a scheduling limit; the next candidate adds coalesced,
  capacity-sensitive queued notifications without increasing any payload bound.
- Candidate expiry origins were source-motion 6,859 / 7,511, source-lag
  575 / 35, and raster-wait 347 / 344. These are simulation-lifetime outcomes,
  not interchangeable with raster failure or actual missing pixels.
- Qt Quick render phase median was about 46.7ms and swap-phase p99 about 105ms.
  mpv render CPU p99 was about 31ms. Candidate property polling p99 was about
  3ms, not the principal observed tail. No mpv behavior change is justified by
  these data. Phase/GL-call CPU durations do not measure GPU elapsed time.
- Normal adaptive-QoS candidate runs intentionally dropped 7,895 / 7,971 source
  comments; all 4,105 / 4,029 admitted IDs reached a draw. Baseline intentionally
  dropped 580 / 611 but only 2,255 / 2,262 IDs reached a draw. These are unequal
  workloads and do not establish a performance gain or complete visible pixels.
- Candidate basic glyph/color probes passed with exact pixels at DPR1 and DPR2,
  including emoji. Wide text (2,357 / 4,714 physical pixels) and finite active
  pressure (385 / 193 comments) failed residency. The next candidate crops only
  invisible margins and tiles wide sprites, retaining full logical geometry and
  the same eight atlas pages, with new seam/fractional/tint/fade probes.
- Atlas-pressure captures had zero wrong pixels, but the old sentinel-page
  assumption failed the intended eviction/replay coverage. The fixture now
  discovers actual page membership, protects an anchor per page and verifies
  an additional replay repack. Pixel tolerances and offered active counts remain
  unchanged; this fixture correction is not recorded as a previous pass.

The next candidate remains unqualified until real GL correctness and whole-frame
comparisons finish. No active sprite is allowed to count as first-drawn with only
some tiles resident. Actual hardware, Wayland/HDR and long-duration qualification
remain outside this software-Mesa CI evidence.

## Third discovery head: `b4fb483`

[Regular CI](https://github.com/sh4869221b/niconeon/actions/runs/36941613279)
passed all five jobs, including real OpenGL and sanitizers with leak checks.
[Performance/pixel CI](https://github.com/sh4869221b/niconeon/actions/runs/36941613265)
failed. Raw [timing](https://github.com/sh4869221b/niconeon/actions/runs/36941613265/artifacts/11201031043)
and [pixel](https://github.com/sh4869221b/niconeon/actions/runs/36941613265/artifacts/11200468510)
artifacts expire after 14 days. Selected raw intervals, summaries and pixel results
are retained in [`evidence-b4fb483`](evidence-b4fb483/results.json).

The unchanged 400 cps / 12,000-comment scenario now accepts, rasterizes and first-draws
**all 12,000 IDs in both trials**, with zero expiry/failure/unresident observations.
This fixes the previous missing-completion result, but does not prove individually
visible pixels in heavily overlapping scenes or real-time throughput. Last draw was
about 39s on the trace clock (measurement starts about 1s into that clock), after the
30s feed. Admission-to-first-draw p99 was 874.942 / 869.474ms; scheduled-comment-time
to first-draw p99 was 8.213 / 8.294s. Source backlog is material and not hidden.

The wake/crop/tile commit does not alter source stamps, simulation speed, pending
motion compensation or expiry thresholds. For example, `perf-11999` in trial1 has
source lag 3.269s, pre-admission motion 4.436s, pending motion 0.338s, and activation
x=-2.923 with width405. It enters partially on-screen at its compensated position;
readiness does not restart its lifetime. The existing simulation clock/200ms cap
remains distinct from a future high-precision media clock.

Candidate feed p95/p99 was 164.457/169.126 and 161.607/169.257ms; baseline was
132.082/143.464 and 129.589/139.086ms but submitted only 3,223 / 3,287 IDs.
The candidate performs much more actual drawing, with up to 2,840 active instances.
These incomplete baseline arms cannot certify an equal-work speedup or regression
estimate. They also cannot be discarded to declare the candidate accepted.
Normal QoS runs intentionally dropped 4,909 / 4,934, admitting/drawing 7,091 / 7,066;
baseline intentionally dropped 440 / 492 but drew only 2,779 / 2,777. Existing
QoS policy is unchanged. No statement here equates draw submission to pixel coverage.

Basic color, eight-page protected repack/replay, finite active385/193, wide ends and
integer internal seam, NG tint and intermediate fade passed at DPR1/2. Fractional
position probes failed: maximum channel error38–149, with the same threshold8.
Qt 6.8.2's [texture destroy implementation](https://github.com/qt/qtbase/blob/v6.8.2/src/opengl/qopengltexture.cpp#L177-L207)
resets filters to Nearest and wrap to Repeat. The previous allocation applied
Linear/ClampToEdge before destroying/recreating its texture, losing those settings.
The next candidate restores the intended sampler after recreation and keeps the
full-image smooth reference and tolerance unchanged. No previous fractional result
is reclassified as passing.

Wake payload bounds held (queued1, outstanding2, max8committed/callback), but trial1
recorded 7,469 notifications / 6,167 no-progress callbacks. New worker completions
were waking a still-full consumer. The next candidate retains the consumer's
count/byte gate across future completions and generation changes, with tests for
capacity-restored wakeup without new data. No payload/queue limit is increased.

Next gate: retain400 and add100/200cps, identical fixture/text/duration, two BA/AB pairs
for discovery. Only equal-complete cases can advance to >=10 pairs and >=1000 feed
frame samples; a lower-rate success does not establish400cps acceptance. Hardware
and all unfinished performance/pixel criteria remain explicitly unqualified.
