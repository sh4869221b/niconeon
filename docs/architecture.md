# Architecture

## Overview

Niconeon is split into two processes.

1. `app-ui` (Qt/QML + libmpv)
2. `niconeon-core` (Rust)

They communicate via JSON-RPC 2.0 over NDJSON on stdio.

## UI Process Responsibilities

- Host `libmpv` as a QML item.
- Control playback (play/pause/seek/volume).
- Send periodic playback ticks (`50ms`) to core.
- Render danmaku overlays and drag/drop interactions.
  - Default backend: Scene Graph aggregate renderer (`DanmakuSceneItem`).
  - Fallback backend: legacy QML delegates (`NICONEON_DANMAKU_BACKEND=legacy`).
- Provide danmaku visibility toggle for low-spec environments.
- Emit periodic UI/danmaku performance logs when enabled.
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
    - UI release build on Windows (MSYS2 + Qt6 + libmpv).
- Release (`.github/workflows/release.yml`)
  - Trigger: `v*` tags and manual dispatch.
  - Packages:
    - source zip.
    - Linux binaries zip (`niconeon-ui`, `niconeon-core`).
    - Linux AppImage (with wrapper setting `NICONEON_CORE_BIN`).
    - Windows binaries zip (Qt runtime + `libmpv` and its dependent MinGW DLLs bundled).
    - `LICENSE`, `COPYING`, `SOURCE_CODE.md`, and `THIRD_PARTY_NOTICES.txt` bundled in distributable binaries.
  - Publishes GitHub Release assets on tag builds and writes `sha256` checksums.
