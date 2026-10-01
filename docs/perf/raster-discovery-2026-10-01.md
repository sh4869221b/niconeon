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
