# Migration regression and qualification plan

CTest is the test entry point. Current build recipes and sanitizer constraints are in
[build-and-validation.md](build-and-validation.md).

## Automated migration coverage

- Domain: filename IDs, Unicode/limits, profile defaults/overrides, bounded synthetic fixtures
- SQLite: split DB roundtrip, legacy cache migration, raw-row preservation, migration failure rollback, platform paths, corrupt data and restart
- Filters: NG-first ordering, regex validation, persistence-before-memory, duplicate/new/latest/single-use Undo, DB failure, empty values and limits
- Timeline: zero/exact time, normal/pause/backward/seek15s window, per-tick cap before coalesce, stable ordering, transactional bounds
- Fetcher: two-stage local mock HTTP, request DTO/headers, response validation, anonymous user, redirects/trust boundary, timeout/cancel/replacement
- Service: network/cache/none, corrupt cache isolation, queue saturation, latest pending open, stale generation/revision, queued shutdown, repeated start/stop
- Controller: file URLs, speed presets/settings, visibility, runtime profile and shutdown
- Qt Quick Test: complete Main.qml import/load, Settings/About/Filter open/close, native C++ types and license resources
- Preserved renderer tests: spatial grid, text width/seek lag, NG rollback, sprite cache
- Packaging: single application executable, required licenses, AppRun local paths and Unicode/spaced arguments

## Actual OpenGL gate (never infer from headless tests)

Build with `NICONEON_BUILD_UI_E2E=ON`, use a working desktop or Xvfb OpenGL display,
and run the `opengl` CTest label. CI explicitly requires OpenGL and no longer silently
skips solely because `GITHUB_ACTIONS` exists. A skipped unsupported backend is not a pass.

G1 manual steps on a formally built `niconeon` and final AppImage/Windows artifact:

1. Initial auto-load of a deterministic local fixture without reopen; confirm media time advances
2. Read actual synthetic comment text on screen (not just active count/FPS)
3. Pause/resume, seek forward/back/zero/same timestamp, hide/show; check text/position/sync
4. Switch file/session while fetching/preparing; reject old work
5. Resize/DPR, Settings/About and picker cancellation, NG drag/outside drop/Undo
6. Close/restart, preserve settings/profile, no orphan subprocess
7. Record artifact hash, OS, Qt/mpv, renderer, GPU/driver and exact passed/unverified scope

## Separate qualification

The migration does not certify all #78 runtime contracts: GUI sprite raster and inherited
renderer caches/queues remain explicit #65/#78 work. 60fps is a target, not inferred from
CTest. Real Windows GPUs, native Wayland (not XWayland), high-refresh/HDR, 8h/24h soak,
10k random seeks and clean end-user distribution licensing belong to #81–#85/#75.

Sanitizer results must say whether address/undefined/thread/leak checks actually ran.
Uninstrumented Qt/system-library reports need diagnosis; do not blanket-suppress application races.
