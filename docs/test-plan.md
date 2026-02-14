# Test Plan (MVP)

## Core Unit Tests

- video id extraction: valid / invalid filename.
- regex validation: valid and invalid patterns.
- filter order: NG user first, then regex.
- undo last NG: only the latest token is restorable.
- `playback_tick_batch`: normal progression, seek reset, paused tick, and legacy method rejection.

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
- コメント非表示時は弾幕描画が停止し、再度表示に戻すと現在再生位置から再同期する。
- 計測ログ有効化時に、2秒ごとに `[perf-ui]` が標準出力へ出力され、`tick_sent`/`tick_result`/`tick_backlog` を含む。
- 計測ログ有効化時に、`[perf-ui]` が `dropped_comments` / `coalesced_comments` / `emit_over_budget` を含む。
- 計測ログ有効化時に、2秒ごとに `[perf-danmaku]` が標準出力へ出力され、`avg_ms`/`p50_ms`/`p95_ms`/`p99_ms`/`max_ms` を含む。
- 画面外に出た弾幕（左外/縦外/フェード完了）は次フレームで表示から外れ、更新対象に残らない。
- 1件ドラッグ中でも、他弾幕は通常どおり移動し、画面外弾幕は継続して除去される。
- `[perf-danmaku]` に `rows_total`/`rows_active`/`rows_free`/`compacted` が出力される。
- `[perf-danmaku]` に `lane_pick_count`/`lane_ready_count`/`lane_forced_count`/`lane_wait_ms_avg`/`lane_wait_ms_max` が出力される。
- 高密度再生で `rows_free` が増えたあと、条件を満たすと `compacted=1` が出力される。
- 高密度再生で `lane_forced_count` が増えても、シーク後の再同期とドロップ復帰（同一レーン優先）が壊れない。
- `QSG_RENDERER_DEBUG=render` および `QT_LOGGING_RULES=\"qt.scenegraph.time.glyph=true\"` のプロファイルでログ取得できる。
- #7 回帰確認として、同一動画・同一区間で `fps` / `p95_ms` / `p99_ms` が悪化しない（目安: 5%以内）ことを確認する。
- #7 回帰確認として、Qt Creator QML Profiler で弾幕オーバーレイの per-frame hot path（Binding/JS）が増加していないことを確認する。
- Glyph warmup は常時ONで動作し、設定UIなしでも再起動後に有効であることを確認する。
- `Glyph warmup ON` 時に `[perf-glyph]` ログが2秒ごとに出力され、`warmup_sent_cp` / `warmup_batches` / `warmup_pending_cp` を含むことを確認する。
- `QT_LOGGING_RULES=\"qt.scenegraph.time.glyph=true\"` と併用時に、`[perf-glyph]` のスパイク窓と glyph ログを突合できることを確認する。
- Glyph warmup 常時ON時に `p95_ms` / `p99_ms` の著しい悪化がないこと、かつ文字化け・欠落がないことを確認する。
- About ダイアログで `LICENSE` / `COPYING` / `THIRD_PARTY_NOTICES` を閲覧できる。
- 既定の `QSGRenderNode` バックエンドで、コメント表示・ドラッグ・NGドロップ・Undo が機能する。
- 高密度区間でドラッグ開始時のヒットテストが安定し、意図しないコメント選択が増えない。
- `NICONEON_DANMAKU_WORKER=on`（既定）で再生・シーク・ドラッグ・NG の回帰がない。
- `NICONEON_DANMAKU_WORKER=off` へ切替後も同等機能が成立し、クラッシュしない。
- `NICONEON_SIMD_MODE=auto/scalar/avx2` で起動し、`[danmaku-simd]` ログが期待モードを示す。
- `NICONEON_SIMD_MODE=avx2` と `scalar` で表示破綻（位置飛び/消去漏れ）がない。
- `just perf-dummy` で #24 前後を比較し、`updates` 同等条件で `avg_ms` または `p95_ms` が悪化していない。
- 連続シーク（10回以上）+ 連続ドラッグ（10回以上）を行っても、worker有効時にクラッシュしない。
- Runtime profile を `high` / `balanced` / `low_spec` に切り替えて、`set_runtime_profile` 応答と挙動（emit cap/coalesce）が一致する。
