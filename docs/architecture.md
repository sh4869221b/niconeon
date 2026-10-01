# C++23 application architecture

`niconeon` is one Qt process and one application executable. There is no subprocess,
stdio transport, JSON-RPC dispatcher, duplicate protocol DTO, or service-style core boundary.

## Layers

- `domain`: value types, validated limits, runtime profile, basename video-ID extraction
- `comments`: asynchronous QNetworkAccessManager lifecycle, API JSON decoding, immutable sorted dataset, cursor, synthetic fixtures
- `filters`: ordered NG-first/regex-second matching and persistent-first mutation/Undo
- `storage`: thread-confined Qt SQL connections and existing profile/cache migration
- `app`: GUI ApplicationController and bounded asynchronous CommentService facade
- `playback` / `danmaku`: retained libmpv and sprite renderer integration
- `ui/qml`: layout, styling, visual state, dialogs, trivial display formatting, signal forwarding

QML contains no HTTP, JSON parsing, database access, player state machine, timeline,
regex matching, scheduling, QoS decisions or renderer ownership. Main.qml forwards
commands to ApplicationController; typed worker results never pass through QML/JSON.
The retained renderer takes a QVariant view adapter constructed in bounded GUI chunks.

## Thread and ownership table

| Owner | Owned resources/work | Boundary |
|---|---|---|
| GUI | ApplicationController, settings, presentation models, user commands, renderer controller | Lightweight state/notifications; no HTTP/large JSON/SQL/filter matching |
| Comment worker QThread | QNetworkAccessManager/replies/deadline, JSON decode, one timeline, FilterManager, two SQLite connections | Commands and typed result signals; all SQL created, used and destroyed here |
| Retained danmaku update worker | SoA simulation state | Existing row-diff/frame contracts; further global bounding remains #78 |
| Qt Quick render thread | libmpv render context, FBO, textures/buffers, rendernode draw | No network/DB/JSON. mpv GL state reset before Quick composition |
| Future raster workers | Whole-comment raster / shaping | #65 blocking follow-up; existing GUI raster is explicitly not qualified |

Network and parsing/storage share one non-GUI worker in this migration. HTTP is asynchronous;
JSON and SQLite can delay other worker commands, but never synchronously execute on GUI/render.
This deliberately avoids a proliferation of unbounded background tasks. Future splitting
requires measurements and the same bounded admission contracts.

## New queue contracts

| Queue | Capacity / producer → consumer | Overflow / cancellation |
|---|---|---|
| Application commands/results | 32 admitted commands, GUI → comment worker → GUI acknowledgement | Mutations reject with visible busy error; admission released only after GUI acknowledgement, bounding queued results too |
| Latest open request | One coalesced pending open in GUI | Newer navigation replaces it even when command queue saturated. Old generation invalidates immediately |
| Latest runtime profile | One coalesced pending profile in GUI | Retained under saturation, admitted before subsequent ticks; shutdown discards it |
| Tick requests | One in flight + one pending latest tick | Coalesce pending position, retain seek flag; revision rejects old seek/visibility result |
| HTTP | One active reply, 1 MiB read buffer, 64 MiB accumulated response | Abort previous on new load; 15s total stage deadline (test injectable); oversize/error/redirect fail closed |
| Dataset | At most 250,000 comments / conservative 64 MiB serialized budget; 16 KiB text, 1 KiB IDs | Reject malformed/oversize dataset with error; cached fallback or empty dataset keeps local video alive |
| Worker tick batch | At most 32 input ticks / 250,000 output records | Explicit transactional failure before cursor commit; production facade emits single coalesced ticks |
| GUI render preparation | Two immutable result batches; at most 64 records converted per drain | Excess batch is dropped with perf/QoS accounting; pending data cleared on seek/hide/new video |

Filters are limited to 10,000 entries, patterns to 4 KiB and IDs to 1 KiB.
No arbitrary method-name or JSON parameter dispatch remains. Filter mutations preserve
persist-first ordering: a failed DB write cannot change in-memory filters or Undo.
Regex execution uses PCRE2 limits (100,000 match steps, 1,000 depth, 1 MiB interpreter heap).
Invalid/resource-exhausted matches are explicit errors, never silently treated as visible comments.
Tick processing checks cancellation between comments and regexes and limits each batch to
100,000 regex evaluations, independent of machine speed. A currently executing regex finishes
within its engine limits before the next check; this is not a hard real-time deadline. Cancellation, seek revision changes, and errors leave the
cursor unchanged and discard partial output. NG-user filtering still short-circuits regex work.

## Generations

- Every video load (including no-ID local video) advances the session generation
- A seek advances the tick revision, invalidates already queued output, resets visible renderer state
- Newest navigation is retained under queue saturation; stale HTTP/parser output cannot commit
- Pending render preparation is cleared on seek/hide/video switch
- Failed mpv seeks are correlated by request ID; stale failures cannot cancel a newer seek.
  One replaceable 5-second timer reconciles comments to the observed media clock if
  a demuxer never lands within the position tolerance; file changes and shutdown cancel it.
- Existing renderer DPR/font/resource invalidation remains part of #65/#78 qualification;
  this document does not claim those old queues are fully bounded or async-raster compliant

## Shutdown

Window close forwards to ApplicationController. It stops producers/timers, clears GUI
render backlog, atomically stops admission and invalidates generation. The worker skips
queued nonstarted commands, aborts HTTP, destroys timeline/filter/SQL on its own thread,
then quits. The GUI event loop stays alive until the worker signals completion; normal
close does not synchronously wait. A bounded 2-second destructor fallback exists for
emergency teardown, retaining a still-running owner rather than destroying its QThread.

The mpv renderer retains shared state independently of QQuickItem lifetime. Render context
and callbacks are released by the render-thread owner before the final handle destruction.
Callbacks target a retained notifier with auto-disconnecting item connections, not a raw item.
First file loads are held until a render-ready notification and submitted asynchronously.

## Observability

`[perf-ui]` exposes admitted queue depth/high-water, stale results, coalesced ticks,
render backlog, emit drops/coalesces/over-budget and target profile. Existing renderer
frame-time/upload metrics are retained. Full per-queue byte/high-water/latency and shutdown
qualification across retained renderer paths remains #78.

## Explicit follow-ups / non-claims

- #65: move retained GUI text raster to bounded workers; no GUI/render worker waits
- #78: all retained raster/update/upload/cache queues, cancellation and shutdown qualification
- #68/#73: high precision media clock and cursor scheduling optimization; a C++ 50ms polling timer remains for compatibility, with no IPC batch transport
- #74: compare production video integration options; current QQuickFramebufferObject remains baseline
- #61/#79/#80: deterministic positions, semantics and text architecture comparison
- #81–#85: measured 60fps/stability/platform/HDR/clean-distribution qualification

The revised #75 order is: #78 design → #59 → #65 → #78 final validation.
These gates are separate from C++ migration correctness and must not be auto-closed by it.
