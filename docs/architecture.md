# Architecture

## Overview

Niconeon is split into two processes.

1. `app-ui` (Qt/QML + libmpv)
2. `niconeon-core` (Rust)

They communicate via JSON-RPC 2.0 over NDJSON on stdio.

## UI Process Responsibilities

- Host `libmpv` as a QML item.
- Control playback (play/pause/seek/volume).
- Enqueue periodic playback ticks (`50ms`) and send them to core as `playback_tick_batch`.
- Render danmaku overlays and drag/drop interactions.
  - Backend: `QSGRenderNode` atlas/sprite renderer (`DanmakuRenderNodeItem`).
    - Default: `NICONEON_DANMAKU_RENDERER=atlas`
    - Fallback: `NICONEON_DANMAKU_RENDERER=frame_image`
  - Simulation update path:
    - Default: worker-thread simulation (`NICONEON_DANMAKU_WORKER=on`).
    - Fallback: single-thread simulation (`NICONEON_DANMAKU_WORKER=off`).
  - SIMD mode for position update:
    - `NICONEON_SIMD_MODE=auto|avx2|scalar` (default: `auto`).
- Provide danmaku visibility toggle for low-spec environments.
- Apply runtime profile (`high` / `balanced` / `low_spec`) and target FPS (`60` by default) to keep playback stable on low-end CPUs.
- Emit periodic UI/danmaku/render performance logs when enabled.
- Show NG drop zone only during drag.
- Show toast notifications and Undo actions.

## Core Process Responsibilities

- Extract video ID from path (regex `(sm|nm|so)\\d+`).
- Fetch comments from NicoNico `nvcomment` API.
- Cache comments in SQLite.
- Keep persistent state for NG users / regex filters.
- Apply filtering in fixed order:
  1. NG user ID
  2. Regex filters
- Resolve tick windows to emitted comments.
- Shape emitted comment burst per tick window by runtime profile (`max_emit_per_tick`, optional coalescing) and return drop/coalesce metrics.

## Failure Handling

- Fetch errors never stop playback.
- If network fetch fails and cache exists, cache is used.
- If neither network nor cache is available, comments are empty and UI is notified.

## CI/CD Responsibilities

- CI (`.github/workflows/ci.yml`)
  - Trigger: pull requests and pushes to `main`.
  - Verifies:
    - License notice generation consistency (`THIRD_PARTY_NOTICES.txt`).
    - Rust core tests (`cargo test` on `core` workspace).
    - UI release build on Linux (Qt6 + libmpv).
    - UI unit tests on Linux (`ui-unit-linux`, headless/offscreen).
    - UI E2E best-effort run on Linux (`ui-e2e-linux-best-effort`; local `just ui-e2e` remains authoritative for rendernode pixel assertions).
    - UI release build on Windows (MSYS2 + Qt6 + libmpv).
  - On pushes to `main`, additionally produces prebuilt release artifacts (`release-linux-binaries`, `release-linux-appimage`, `release-windows-binaries`) for 14-day retention.
- Release (`.github/workflows/release.yml`)
  - Trigger: `v*` tags.
  - Waits for the matching successful `main` CI run for the same SHA, downloads the prebuilt Linux/Windows artifacts, re-verifies bundled licenses, writes `sha256` checksums, and publishes the GitHub Release.
  - Builds only the source zip at release time.
  - Repository:
    - Full changelog markdown (`CHANGELOG.md`, generated from commit subjects and auto-updated on tag releases).
  - Publishes GitHub Release assets from promoted artifacts instead of rebuilding them.
- Manual rebuild (`.github/workflows/release-rebuild.yml`)
  - Trigger: `workflow_dispatch`.
  - Re-runs the full Linux/Windows packaging pipeline when promoted artifacts are unavailable or stale, then publishes the same release asset set as the normal release workflow.
