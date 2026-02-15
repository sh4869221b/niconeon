# Changelog

_Generated from git commit subjects (merge commits excluded)._

## v0.0.16 (2026-02-15)

_Range: `v0.0.15..v0.0.16`_

- feat(ui): incremental danmaku spatial updates
- fix(ui): prevent danmaku text clipping
- fix(ui): make theme dark-mode-safe across Windows controls
- chore(release): auto-update repository changelog on tag release

## v0.0.15 (2026-02-15)

_Range: `v0.0.14..v0.0.15`_

- refactor(ui): remove unused danmaku model path and dead files
- feat(perf): add cli dummy profiling workflow
- feat(core): add runtime profile and emit shaping
- perf(ui): add QoS profile controls and reduce danmaku overhead
- fix(ui): make glyph warmup always on
- perf(ui): reuse worker frame buffers and reduce SoA copies
- fix(ui): apply rendernode model transform
- test(ui): add rendernode e2e and run it in CI
- fix(ci): install QtQml WorkerScript module for ui e2e
- fix(test): force OpenGL backend in rendernode e2e
- fix(test): stabilize rendernode e2e under CI
- fix(test): keep e2e graphics API labels Qt6.4-compatible
- fix(test): skip rendernode pixel assertion on GitHub Actions

## v0.0.14 (2026-02-14)

_Range: `v0.0.13..v0.0.14`_

- feat(protocol,ui): replace playback_tick with playback_tick_batch
- feat(ui): migrate danmaku overlay to QSGRenderNode renderer
- docs: update R3 renderer and tick-batch behavior
- fix(ui): clip danmaku overlay and drop legacy shader fallback

## v0.0.13 (2026-02-14)

_Range: `v0.0.12..v0.0.13`_

- fix(ui): pass worker frame via single FrameInput for Qt 6.4

## v0.0.12 (2026-02-14)

_Range: `v0.0.11..v0.0.12`_

- docs(agents): add policy to avoid release monitoring by default
- perf(ui): add SoA+SIMD worker update pipeline
- docs(perf): add R2 runtime modes and validation matrix

## v0.0.11 (2026-02-14)

_Range: `v0.0.10..v0.0.11`_

- perf(ui): reduce QML per-frame danmaku overhead
- perf(ui): add glyph warmup and spike metrics
- feat(ui): add scenegraph danmaku backend and spatial drag grid
- docs(perf): document R1 backend switch and validation flow

## v0.0.10 (2026-02-14)

_Range: `v0.0.9..v0.0.10`_

- feat(perf): add frametime distribution logging and measurement guide
- fix(ui): keep button text readable on dark theme
- docs: require closes/fixes issue keyword in commit messages
- perf(ui): pool offscreen danmaku rows and stop updates
- ci(windows): speed up release and ui builds
- perf(ui): switch lane assignment to reservation model
- fix(ui): move file path input below control buttons

## v0.0.9 (2026-02-11)

_Range: `v0.0.8..v0.0.9`_

- perf(ui): reduce danmaku overhead and add perf logging controls

## v0.0.8 (2026-02-11)

_Range: `v0.0.7..v0.0.8`_

- fix(ui): force windows opengl and harden core process startup

## v0.0.7 (2026-02-11)

_Range: `v0.0.6..v0.0.7`_

- fix(ui): normalize file urls and validate video path in mpv loader

## v0.0.6 (2026-02-11)

_Range: `v0.0.5..v0.0.6`_

- fix(ui): normalize file dialog url to local path

## v0.0.5 (2026-02-11)

_Range: `v0.0.4..v0.0.5`_

- fix(ui): correct windows file dialog path and settings metadata

## v0.0.4 (2026-02-11)

_Range: `v0.0.3..v0.0.4`_

- fix(release): add qt.conf for windows qml imports

## v0.0.3 (2026-02-11)

_Range: `v0.0.2..v0.0.3`_

- fix(release): bundle transitive qt runtime dlls on windows

## v0.0.2 (2026-02-11)

_Range: `v0.0.1..v0.0.2`_

- feat(ui): add about dialog with embedded license texts
- chore(release): enforce license metadata in artifacts and CI
- chore(repo): ignore app-ui build-ci directory
- fix(release): resolve appimage verify path from temp dir

## v0.0.1 (2026-02-11)

_Range: `v0.0.1`_

- feat: bootstrap niconeon MVP desktop app
- chore: add just task runner commands
- fix: align danmaku sync with pause/seek and correct video orientation
- feat: add playback speed presets and toggle control
- fix(ui): stabilize danmaku drag drop and ng zone behavior
- docs: add repository AGENTS guide
- fix(ui): make ng drop detection reliable during drag
- feat(filter): allow removing ng users from ui
- feat(ui): highlight ng drop target overlap while dragging
- chore: ignore release build and dist artifacts
- ci: add github actions for checks and release packaging
- fix(ci): align Qt requirement and MSYS2 mpv package
- fix(ui): support Qt 6.4 QML module loading
- fix(release): use valid MSYS2 mpv package
- fix(release): copy runtime DLLs before windeployqt
- fix(release): tolerate windeployqt local dep scan errors
- fix(release): make windeployqt find qmlimportscanner on msys2

