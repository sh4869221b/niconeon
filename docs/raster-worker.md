# Bounded whole-comment raster worker (#65)

## Ownership and publication

One CPU worker owns `QFontMetrics`, shaping, independent `QImage` storage and `QPainter::drawText`.
The GUI captures the application `QFont` value and supplies immutable text/size/DPR values; it
never measures or paints text, including cache misses in `appendComments` and `onFrame`.
The GUI index is not shared with the worker. Shared queue locks cover only bookkeeping and
value moves; no lock is held while measuring/painting, and GUI/render consumers never wait
for an unfinished raster. Images are published only by the GUI's bounded completion drain.
OpenGL remains exclusively on the Qt Quick render thread. The `frame_image` fallback still
composites already-rasterized images with QPainter; it does not shape/rasterize text.

## Bounds and pressure

| Boundary | Hard bounds | Pressure behavior |
|---|---|---|
| GUI → raster, including in-flight and undrained completions | 128 current-generation unique requests and 4 MiB UTF-16 text | Return no admission; producer retries the unaccepted suffix |
| CPU raster allocation | One image, at most 8 MiB | Invalid/oversize/failed raster produces a terminal failed completion and visible diagnostic, never an infinite retry |
| Worker → GUI completion queue | 32 entries and 8 MiB images | Worker waits on its own condition variable, interruptible by generation/stop; one currently painted/held image is additional |
| GUI → render upload mailbox | One batch: at most 32 sprites and 2 MiB target | GUI callbacks commit at most 8 completions each, stopping at mailbox count/byte capacity; one image exceeding the target is admitted only to an empty mailbox, hard maximum 8 MiB |
| Awaiting-comment activation | 256 comment records | Stop admission; duplicate text shares raster work but cannot grow this queue without limit |
| Source batch retry | Existing two immutable, validated dataset-bounded batches; 64 conversion attempts/drain | Offset advances only by the returned accepted prefix; regular timeline ticks pause while the previous batch awaits admission |

The per-callback GUI commit budget (8) is separate from the render mailbox capacity (32).
Several short GUI callbacks may fill that one mailbox; a slower presentation clock must not
artificially cap throughput at eight images per presented frame. The 2 MiB target limits
burst size, while keeping backpressure at the mailbox rather than creating more mailboxes.
This scheduling choice does not certify the whole-page atlas's high-density frame tails.

No unbounded Qt queued raster invocations or per-completion signals are used. Worker results
are polled by the existing frame timer, including during pause. Input text is limited to
16,384 UTF-16 units (compatible with the domain's 16 KiB UTF-8 limit); raster font sizes are
1–256 px and finite DPR is 1–16. Byte arithmetic is checked before image allocation.

The whole-page atlas upload mechanism remains unchanged: at most eight 2048×2048 RGBA pages
(128 MiB) can be dirtied/uploaded in one render frame. The 2 MiB mailbox target describes
sprite transfer, not actual GL page traffic. Partial-page uploads are #62, not claimed here.
CPU images/ready-key metadata retained by the renderer/cache are **not** a new byte-budgeted
LRU; that separate #64 work is still required. This patch bounds outstanding work, not every
long-lived renderer allocation or the existing simulation worker's command queues (#78).

## Time, ordering and interaction

New comments remain outside render snapshots and hit testing until their complete sprite
and exact measured width have reached the upload mailbox. Accepted order is retained.
No placeholder or blank quad is used to pretend text is ready. The source batch's existing
media-position stamp and an accumulated simulation-motion timestamp survive retries.
Deferred positions advance using exactly the retained simulation's pause/rate and worker
200 ms step cap/coalescing rules, and activation occurs before the next motion step.
This preserves existing timing rather than claiming the future high-precision media clock
of #68/#61. A comment already expired at readiness is counted as `expired`, not respawned late;
supported pressure tests must show all unexpired admitted comments and zero raster failures.
Invalid comment IDs are consumed (not retried forever). Existing profile/QoS drops remain
explicit source behavior; raster pressure itself does not silently drop an accepted suffix.

Pending NG-user decisions block activation for that user. A successful persistent NG removes
its queued comments, with unadmitted source-batch rows excluded lazily in the existing 64-row drain; a failed write unblocks the
retained comments and restores existing fades. Seek/hide/session replacement cancels pending
comment records. Font/DPR replacements leave existing readable images in use until the new
ID and width can be swapped together. Empty/error raster results are never published as ready.

## Generation and shutdown

`clear` changes generation and cancels queued/held work. `cancelPending` also changes generation
but retains already-published immutable sprites, so repeatedly seeking the same text does not
allocate a fresh renderer image each time. Their bounded upload mailbox must remain intact:
readiness cannot survive while its only unconsumed pixels are discarded. Sprite IDs are monotonic.
A cancelled in-flight request may temporarily add one bounded old image/text value outside the new generation counts.
A worker that finishes after cancellation cannot modify the GUI index or publish a stale result.
Application-font events and DPR changes regenerate the index; media seeks reuse matching images.

Normal application close stops the frame producer, rejects admission, clears pending comments,
cancels queues and wakes the worker. ApplicationController keeps its 16 ms shutdown poll alive
until both comment-service and raster workers stop, then permits quit. There is no normal GUI
join wait. Direct/emergency owner destruction joins the one current bounded paint before destroying
its state; it never needs queued GUI events and does not drain the cancelled backlog. This is not
a hard-real-time deadline or a claim that every inherited renderer destructor is nonblocking.

## Tests and measurement

`danmaku_sprite_cache_test`: duplicate pending keys, bounded request/completion queues, stalled
consumer, cancellation during paint, font generation, failure injection, oversize input, stop,
repeated start/cancel/stop, cached seek reuse and pixel-for-pixel reference comparison for Japanese,
combining marks, Arabic and emoji/ZWJ at DPR 1/1.5/2.

`danmaku_raster_pipeline_test`: 350 unique comments through stalled render pressure with accepted
prefix retries, count/byte limits, every sprite delivered once and every comment represented,
bounded duplicate records, stale seek cancellation, repeated seek reuse, pending-upload DPR swap,
NG persistence success/failure, pause/rate delay and a >200 ms GUI stall.

The opt-in `NICONEON_BUILD_PERF_TOOLS=ON` builds `raster_profile`. It exercises the real controller
with a deterministic Japanese/mixed unique or 32-string warm workload, a 16 ms GUI heartbeat,
and a bounded mock render consumer. It excludes video/GPU/QoS, and reports offered/accepted/upload
counts, heartbeat p95/p99 and admission-call p95/p99. The same source compiles against the baseline's
void admission API. Do not equate these CPU scheduling results with presented-frame/GPU qualification.
Actual app runs must separately retain identical video/hash, profile, workload, software/hardware
backend and duration, and disclose runtime QoS changes and dropped/expired counts.
