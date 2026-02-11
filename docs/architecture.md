# Architecture

## Overview

Niconeon is split into two processes.

1. `app-ui` (Qt/QML + libmpv)
2. `niconeon-core` (Rust)

They communicate via JSON-RPC 2.0 over NDJSON on stdio.

## UI Process Responsibilities

- Host `libmpv` as a QML item.
- Control playback (play/pause/seek/volume).
- Send periodic playback ticks (`33ms`) to core.
- Render danmaku overlays and drag/drop interactions.
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
