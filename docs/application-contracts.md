# In-process application contracts

C++ `domain/Domain.hpp` is the value-type boundary. It replaces the removed stdio JSON-RPC protocol.
QObject signals/slots cross thread boundaries using registered typed values. QML sees only
presentation properties and commands on ApplicationController.

## Comment/session behavior

- Filename-only video ID extraction, case-insensitive `sm|nm|so` plus decimal digits
- One active comment dataset; every new video invalidates prior pending work
- Normal window emits `lastPosition < atMs <= position`; initial position is -1 so 0ms is emitted
- Seek/backward restores `[position−15000, position)`, including during pause; exact-position comments remain for the next normal tick
- Paused normal tick emits nothing; session position advances
- NG users precede regular expressions
- Per-tick emission cap precedes batch-wide coalescing by `(atMs,userId,text)`; first occurrence is retained
- `high`: 60fps, unlimited profile cap; `balanced`:60fps/96; `low_spec`:60fps/48/coalesce
- Explicit overrides clamp fps to10–120 and nonzero profile cap to2000. Safety dataset/batch limits apply independently

## Persistence and errors

Existing schema/path migration is described in README. Network success can still display
comments if cache persistence fails. HTTP failure uses valid cache, then empty comments.
If cache initialization or legacy cache migration fails, the worker warns once and runs
without the cache. It continues loading and saving filters in the persistent data DB.
Corrupt cache never stops a local video. SQL errors carry operation context.
For each persistent DB, the worker checks the journal mode and retries contention during
WAL setup for up to two seconds. An already migrated data DB in WAL mode opens with
read-only schema checks. When the schema needs work, the worker checks it again after
taking the SQLite writer lock.
NG users and regex filters share a 10,000-row limit. Each insertion checks the limit
and writes in one SQLite write transaction. A duplicate NG user remains a no-op at
capacity. The concurrency guarantee requires every writer to use the updated application.
Startup reads the NG and regex lists in one SQLite read transaction and applies that
committed snapshot to the filter engine.
Only the latest NG registration actually inserted into SQLite by this instance gets a
single-use Undo token. A duplicate uses a read-only lookup of the committed row,
synchronizes the local NG filter and preserves any existing token. If the row is absent,
the worker rechecks after taking the writer lock. A deletion committed after the lookup
may leave the local filter active until a local removal or a fresh snapshot load.
Removing an NG user in the same instance invalidates its Undo.
Each new row stores its own registration ID, which is also its Undo token. Undo deletes
only a row matching both the user ID and the registration ID. If another instance
deleted or replaced the row, the old token expires and the local filter stays active.
The additive SQLite migration preserves older rows with a NULL registration ID.
The Undo guard applies to instances running the updated application.
Invalid regex is checked before database write. Qt uses PCRE2 semantics and Unicode properties.
If the data DB cannot be opened or a persisted filter is invalid, comment emission fails
closed with one explicit warning at initialization. Local video playback continues.
Subsequent ticks acknowledge an empty, unprocessed batch without repeating the warning,
including while paused, seeking or opening another video.
Filter mutations report failure while the persistent filter engine is unavailable.

## HTTP policy

Production watch/comment hosts are HTTPS `nicovideo.jp` or its subdomains, with no URL
userinfo/fragments. Redirects and TLS errors are not silently accepted. Cookie use retains
the existing `NICONICO_COOKIE` input (`NICONEON_NICONICO_COOKIE` is an optional fallback alias) and sends it only to a validated HTTPS comment
endpoint; logs never include cookies, thread keys, response bodies or account URLs.
Tests inject HTTP loopback endpoints explicitly and do not send cookie fixtures over HTTP.
No account credentials or live authenticated NicoNico calls are needed by tests.

## View contract

QML dialog open/close, visual drag gesture forwarding, viewport coordinates, toast timeout
and display formatting are presentation responsibilities. C++ owns speed normalization,
persistence, profile/QoS, filtering, timeline and all playback/comment state transitions.
Main.qml places the toast above the player and comment overlay so it remains visible and
its Undo button receives input in the playback area.
