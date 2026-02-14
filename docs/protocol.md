# JSON-RPC Protocol (NDJSON)

Each line is one JSON-RPC 2.0 message.

## Methods

### `ping`
- params: `{}`
- result: `{ "ok": true }`

### `open_video`
- params:
  - `video_path: string`
  - `video_id: string`
- result:
  - `session_id: string`
  - `comment_source: "cache" | "network" | "none"`
  - `total_comments: number`

### `playback_tick_batch`
- params:
  - `session_id: string`
  - `ticks: PlaybackTickSample[]`
- result:
  - `emit_comments: CommentEvent[]`
  - `processed_ticks: number`
  - `last_position_ms: number`
  - `dropped_comments: number`
  - `coalesced_comments: number`
  - `emit_over_budget: boolean`

### `set_runtime_profile`
- params:
  - `profile: "high" | "balanced" | "low_spec"`
  - `target_fps?: number` (10..120)
  - `max_emit_per_tick?: number` (`0` は無制限)
  - `coalesce_same_content?: boolean`
- result:
  - `profile: string`
  - `target_fps: number`
  - `max_emit_per_tick: number`
  - `coalesce_same_content: boolean`

### `add_ng_user`
- params:
  - `user_id: string`
- result:
  - `applied: boolean`
  - `undo_token: string`
  - `hidden_user_id: string`

### `remove_ng_user`
- params:
  - `user_id: string`
- result:
  - `removed: boolean`
  - `user_id: string`

### `undo_last_ng`
- params:
  - `undo_token: string`
- result:
  - `restored: boolean`
  - `user_id: string | null`

### `add_regex_filter`
- params:
  - `pattern: string`
- result:
  - `filter_id: number`

### `remove_regex_filter`
- params:
  - `filter_id: number`
- result:
  - `removed: boolean`

### `list_filters`
- params: `{}`
- result:
  - `ng_users: string[]`
  - `regex_filters: RegexFilter[]`

## Types

```json
CommentEvent {
  "comment_id": "string",
  "at_ms": 123,
  "user_id": "string",
  "text": "string"
}

RegexFilter {
  "filter_id": 1,
  "pattern": "foo.*bar",
  "created_at": "2026-02-11T00:00:00Z"
}

PlaybackTickSample {
  "position_ms": 1234,
  "paused": false,
  "is_seek": false
}
```
