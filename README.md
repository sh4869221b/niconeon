# Niconeon

Niconeon は、ローカル動画を再生しながらニコニココメントを時刻同期で弾幕表示する Windows / Linux 向けデスクトップアプリです。

## MVP 機能

- ローカル動画再生（再生/一時停止、シーク、音量）
- 再生速度変更（トグル切替 + プリセット編集、前回値保持）
- ニコニココメント自動取得（`sm|nm|so + 数字` の動画IDをファイル名から抽出）
- 再生時刻同期の弾幕オーバーレイ
- コメント表示/非表示トグル（低負荷化用）
- マウスドラッグで NG ユーザー登録（NG ドロップゾーン）
- NG ユーザー即時反映 + Undo（直近1件）
- 正規表現フィルタの登録/削除
- 計測ログ（UI/弾幕更新 + フレーム時間分布: avg/p50/p95/p99/max）
- Glyph warmup（初出文字の段階プリウォーム）ON/OFF
- SQLite 永続化（NG/正規表現/コメントキャッシュ）
- About ダイアログでライセンス情報表示（MIT / GPLv3+ / Third-Party Notices）

## 構成

- `app-ui/` : Qt 6 + QML + libmpv 埋め込み UI
- `core/` : Rust 製コア（取得、キャッシュ、フィルタ、永続化、JSON-RPC）
- `docs/` : 設計・プロトコル・テスト計画
- `docs/licensing.md` : ライセンス方針と再生成手順

## 開発要件

- Rust stable
- CMake 3.21+
- Qt 6.4+
- libmpv
- just（任意だが推奨）

## タスク実行（推奨）

`just` を使うと、Core/UI を横断するコマンドを共通化できます。

```bash
# 一覧
just

# ライセンス通知ファイル再生成
just licenses

# Core テスト
just core-test

# 全体ビルド（core + ui）
just build

# 起動（NICONEON_CORE_BIN を自動設定）
just run
```

`just build` / `just run` は `THIRD_PARTY_NOTICES.txt` を先に自動生成します。

## Core の起動

```bash
cd core
cargo run -p niconeon-core -- --stdio
```

## Core テスト

```bash
cd core
cargo test
```

## UI のビルド（例）

```bash
cd app-ui
cmake -S . -B build
cmake --build build
```

## CI

GitHub Actions で以下を実行します。

- `core-test`（Rust core のテスト）
- `ui-build-linux`（Linux で UI リリースビルド）
- `ui-build-windows`（Windows/MSYS2 で UI リリースビルド）

`main` への PR と `main` への push で実行され、`main` マージ時は必須チェックとして扱います。

## リリース成果物

`vX.Y.Z` タグを push すると、Release ワークフローが以下を生成・公開します。

- `niconeon-X.Y.Z-source.zip`
- `niconeon-X.Y.Z-linux-x86_64-binaries.zip`
- `niconeon-X.Y.Z-linux-x86_64.AppImage`
- `niconeon-X.Y.Z-windows-x86_64-binaries.zip`
- `niconeon-X.Y.Z-sha256sums.txt`

手動実行（`workflow_dispatch`）時は GitHub Release は作成せず、同名成果物を workflow artifact として保存します。

## 制約

- コメント自動取得は、動画ファイル名にニコニコ動画ID（例: `sm9`, `so123456`）が含まれる前提です。
- ID 抽出に失敗した場合、動画再生は継続し、コメントは表示されません。

## 低スペック端末での推奨設定

- `コメント非表示` ボタンで弾幕描画を停止すると、CPU負荷を下げられます。
- `計測ログ開始` ボタンで、2秒ごとの UI 計測ログ（`tick_sent`/`tick_result`/`tick_backlog`）と弾幕ログ（`fps`/`p95`/`p99` など）を標準出力に出せます。
- `Glyph warmup ON/OFF` で、文字生成スパイク対策のプリウォームを切り替えできます（既定: ON）。
- 計測プロファイル（baseline / scenegraph / glyph / combined）は `docs/performance-measurement.md` を参照してください。

## 弾幕描画バックエンド

- 既定は `scenegraph`（`DanmakuSceneItem + updatePaintNode()`）です。
- 障害切り分け用に `legacy` バックエンド（`Repeater + DanmakuItem`）へ切り替えられます。

```bash
NICONEON_DANMAKU_BACKEND=legacy just run
```

## 弾幕更新モード（R2）

- `NICONEON_DANMAKU_WORKER`:
  - 既定 `on`（ワーカースレッド更新）
  - `off` で単スレッド更新へフォールバック
- `NICONEON_SIMD_MODE`:
  - 既定 `auto`（AVX2 対応CPUで `avx2`、それ以外は `scalar`）
  - 明示指定: `avx2` / `scalar`

```bash
# 例: 安全系（単スレッド + scalar）
NICONEON_DANMAKU_WORKER=off NICONEON_SIMD_MODE=scalar just run
```

## ライセンス

- 本リポジトリの自作ソースコードは `MIT` ライセンスです（`LICENSE`）。
- 配布バイナリは `GPL-3.0-or-later` 条件で提供します（`COPYING`）。
- 対応ソースコードの入手方法は `SOURCE_CODE.md` に記載します。
- 依存ライセンス情報は `THIRD_PARTY_NOTICES.txt` に集約しています。
- 配布バイナリには最低限 `LICENSE` / `COPYING` / `SOURCE_CODE.md` / `THIRD_PARTY_NOTICES.txt` を同梱します。
