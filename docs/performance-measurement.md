# Performance Measurement Guide

## Goal

Issue `#4` の目的は、同一条件で再現可能なログを取り、ボトルネックを定量比較できる状態にすることです。

## Prerequisites

- `just build` または以下でビルド済みであること:
  - `cd core && cargo build -p niconeon-core`
  - `cd app-ui && cmake -S . -B build && cmake --build build -j`
- 動画ファイル名にニコニコ動画ID（`sm|nm|so + 数字`）が含まれていること
- 初回再生でコメントキャッシュが作成されること

## Reproducibility Rules

1. 初回実行でコメントを取得し、同じ動画を2回目以降に再生する。
2. トーストに `comment_source: cache` が出る状態で計測する。
3. 同じ区間（例: 0秒〜60秒）を毎回測定する。
4. 速度設定は固定（例: `1.0x`）にする。
5. 比較時は同一プロファイルで2回以上採取する。

## Metrics to Compare

- UI: `tick_sent`, `tick_result`, `tick_backlog`
- Danmaku: `fps`, `avg_ms`, `p50_ms`, `p95_ms`, `p99_ms`, `max_ms`, `updates`, `removed`
- Pool状態: `rows_total`, `rows_active`, `rows_free`, `compacted`
- Lane状態: `lane_pick_count`, `lane_ready_count`, `lane_forced_count`, `lane_wait_ms_avg`, `lane_wait_ms_max`
- Scene Graph: batch/upload 関連ログ
- Glyph: glyph time ログのスパイク有無

## Workload Profiles

### Normal

- 日常利用の動画区間（通常コメント密度）を対象にする。
- 目安として `[perf-danmaku]` の `active` が低〜中程度の区間を選ぶ。

### High Density

- コメントが集中する区間を対象にする。
- 目安として `[perf-danmaku]` の `active` が Normal より明確に高い区間を選ぶ。
- 比較時は「同じ動画・同じ区間」を固定する。

## Profile Commands (Linux)

すべて `LC_NUMERIC=C` を付与してください。

### 1) baseline

```bash
LC_NUMERIC=C \
NICONEON_CORE_BIN="$PWD/core/target/debug/niconeon-core" \
./app-ui/build/niconeon-ui 2>&1 | tee perf-baseline.log
```

### 2) scenegraph

```bash
LC_NUMERIC=C \
QSG_RENDERER_DEBUG=render \
NICONEON_CORE_BIN="$PWD/core/target/debug/niconeon-core" \
./app-ui/build/niconeon-ui 2>&1 | tee perf-scenegraph.log
```

### 3) glyph

```bash
LC_NUMERIC=C \
QT_LOGGING_RULES="qt.scenegraph.time.glyph=true" \
NICONEON_CORE_BIN="$PWD/core/target/debug/niconeon-core" \
./app-ui/build/niconeon-ui 2>&1 | tee perf-glyph.log
```

### 4) combined

```bash
LC_NUMERIC=C \
QSG_RENDERER_DEBUG=render \
QT_LOGGING_RULES="qt.scenegraph.time.glyph=true" \
NICONEON_CORE_BIN="$PWD/core/target/debug/niconeon-core" \
./app-ui/build/niconeon-ui 2>&1 | tee perf-combined.log
```

### 5) worker off + scalar (fallback baseline)

```bash
LC_NUMERIC=C \
NICONEON_DANMAKU_WORKER=off \
NICONEON_SIMD_MODE=scalar \
NICONEON_CORE_BIN="$PWD/core/target/debug/niconeon-core" \
./app-ui/build/niconeon-ui 2>&1 | tee perf-worker-off-scalar.log
```

### 6) worker on + avx2 (R2 fast path)

```bash
LC_NUMERIC=C \
NICONEON_DANMAKU_WORKER=on \
NICONEON_SIMD_MODE=avx2 \
NICONEON_CORE_BIN="$PWD/core/target/debug/niconeon-core" \
./app-ui/build/niconeon-ui 2>&1 | tee perf-worker-on-avx2.log
```

## Operation Procedure

1. アプリを起動して対象動画を開く。
2. 「計測ログ開始」を押す。
3. 指定区間（例: 60秒）を再生する。
4. 必要なら一時停止・シークなしのケースを先に取得する。
5. ログを保存し、次のプロファイルで同じ手順を繰り返す。

## Issue #5 Comparison Focus

- 同一動画・同一区間で、以下を #5 前後で比較する:
  - `updates`（過剰更新が減っているか）
  - `p95_ms` / `p99_ms`（フレーム時間スパイクが改善しているか）
  - `rows_active` と `rows_free`（画面外返却が機能しているか）
- 高密度区間で `compacted=1` が出ることを確認し、compaction後にドラッグ/NG操作の回帰がないことを確認する。

## Issue #6 Comparison Focus

- 同一動画・同一区間で、以下を #6 前後で比較する:
  - `lane_pick_count` に対する `lane_forced_count` の割合
  - `lane_wait_ms_avg` / `lane_wait_ms_max`
  - `p95_ms` / `p99_ms`
- 目視回帰:
  - ドロップ復帰時の同一レーン優先が維持されること
  - 高密度時でも明らかな不自然偏り（特定レーン集中）がないこと

## Issue #7 Comparison Focus

- 同一動画・同一区間で、以下を #7 前後で比較する:
  - `[perf-danmaku]` の `updates`（同等以下であること）
  - `fps` / `p95_ms` / `p99_ms`（悪化しないこと）
- Qt Creator QML Profiler が利用できる場合:
  - 弾幕オーバーレイの毎フレーム評価バインディングが減っていること
  - ドラッグ中・シーク後でも hot path が過度に増えないこと
- 目視回帰:
  - NGドロップ判定、ゾーン外ドロップ復帰、Undo が従来通り動作すること
  - 1件ドラッグ中に他コメントが流れ続けること

## Issue #8 Comparison Focus

- 同一動画・同一区間で、`Glyph warmup OFF` と `Glyph warmup ON` を比較する。
- 2回以上採取し、以下を比較する:
  - `[perf-danmaku]` の `p95_ms` / `p99_ms`
  - `[perf-glyph]` の `new_cp_total` / `new_cp_non_ascii`
  - `[perf-glyph]` の `warmup_sent_cp` / `warmup_batches` / `warmup_pending_cp`
- `QT_LOGGING_RULES=\"qt.scenegraph.time.glyph=true\"` のログと `[perf-glyph]` を同時確認し、スパイク窓（高p99）の再現条件を記録する。
- 受け入れ判定:
  - `ON` で `p99_ms` が悪化しないこと
  - 文字化けや欠落がないこと
  - `warmup_enabled=1` で `warmup_sent_cp > 0` が観測できること

## Issue #9 Comparison Focus

- 同一動画・同一区間で、R1以前のタグと現行タグを比較する（旧 `legacy` バックエンド比較は廃止）。
- 比較対象:
  - `[perf-danmaku]` の `fps` / `p95_ms` / `p99_ms`
  - `[perf-danmaku]` の `updates` / `removed`
  - ドラッグ中に他弾幕が停止しないこと、NG ドロップ登録が機能すること
- 受け入れ判定（R1）:
  - 性能値は参考記録（閾値でブロックしない）
  - 機能回帰がないことを必須とする

## Issue #11 Comparison Focus

- 高密度区間でドラッグ開始の取りやすさ（ヒットテスト）を確認する。
- `drop` での復帰（同一レーン優先）と NG ドロップ判定が維持されることを確認する。
- 連続ドラッグ時に誤判定（掴めない/別コメントを掴む）が増えていないことを確認する。

## Issue #12 Comparison Focus

- SoA 更新経路導入後、同一動画・同一区間で `p95_ms` / `p99_ms` を比較する。
- `rows_active` が高い区間で `updates` と `tick_backlog` の悪化がないことを確認する。
- 機能回帰（ドラッグ/NG/シーク同期）がないことを優先判定にする。

## Issue #13 Comparison Focus

- `NICONEON_SIMD_MODE=scalar` と `NICONEON_SIMD_MODE=avx2` を同条件で比較する。
- 比較対象:
  - `[perf-danmaku]` の `p95_ms` / `p99_ms`
  - `[perf-ui]` の `tick_backlog`
- 受け入れ判定:
  - `avx2` が `scalar` より悪化しないこと（目安: 5%以内）
  - 表示結果（弾幕位置/消去タイミング）が体感一致すること

## Issue #14 Comparison Focus

- `NICONEON_DANMAKU_WORKER=on` と `off` を同条件で比較する。
- 比較対象:
  - `[perf-ui]` の `tick_backlog`
  - `[perf-danmaku]` の `p95_ms` / `p99_ms`
- 受け入れ判定:
  - worker `on` 既定で機能回帰がないこと
  - `off` で即時フォールバック可能であること

## Expected Log Prefixes

- `[perf-ui] ...`
- `[perf-danmaku] ...`
- `[perf-glyph] ...`
- `QSG_RENDERER_DEBUG=render` 由来の renderer ログ
- `qt.scenegraph.time.glyph` 由来の glyph ログ
