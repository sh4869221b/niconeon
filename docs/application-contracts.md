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
Corrupt cache never stops a local video. SQL errors carry operation context; persistence
failure cannot create a false successful NG/Undo result.
Only the latest successful new NG registration has a single-use Undo token. Duplicate
registration does not replace that token. Removing the same NG invalidates its Undo.
Invalid regex is checked before database write. Qt uses PCRE2 semantics and Unicode properties.

## HTTP policy

Production watch/comment hosts are HTTPS `nicovideo.jp` or its subdomains, with no URL
userinfo/fragments. Redirects and TLS errors are not silently accepted. Cookie use retains
the explicit `NICONEON_NICONICO_COOKIE` input and sends it only to a validated HTTPS comment
endpoint; logs never include cookies, thread keys, response bodies or account URLs.
Tests inject HTTP loopback endpoints explicitly and do not send cookie fixtures over HTTP.
No account credentials or live authenticated NicoNico calls are needed by tests.

## View contract

QML dialog open/close, visual drag gesture forwarding, viewport coordinates, toast timeout
and display formatting are presentation responsibilities. C++ owns speed normalization,
persistence, profile/QoS, filtering, timeline and all playback/comment state transitions.
