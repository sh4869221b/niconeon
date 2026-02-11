# Test Plan (MVP)

## Core Unit Tests

- video id extraction: valid / invalid filename.
- regex validation: valid and invalid patterns.
- filter order: NG user first, then regex.
- undo last NG: only the latest token is restorable.
- tick window emission: normal progression and seek reset.

## Core Integration Tests

- cache read/write roundtrip.
- open_video returns `network` when fetched, `cache` on fallback, `none` on no data.

## UI Manual Tests

- playback controls update position and volume.
- drag freezes only grabbed comment.
- NG drop zone appears only while dragging.
- dropping in zone registers NG and fades matching comments in ~300ms.
- dropping outside zone returns comment using same-lane-first behavior.
- undo restores last NG user.
- removing an NG user from filter dialog updates list and future filtering.
- regex invalid input shows error and is not registered.
- About ダイアログで `LICENSE` / `COPYING` / `THIRD_PARTY_NOTICES` を閲覧できる。
