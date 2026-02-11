# Niconeon

Niconeon は、ローカル動画を再生しながらニコニココメントを時刻同期で弾幕表示する Windows / Linux 向けデスクトップアプリです。

## MVP 機能

- ローカル動画再生（再生/一時停止、シーク、音量）
- 再生速度変更（トグル切替 + プリセット編集、前回値保持）
- ニコニココメント自動取得（`sm|nm|so + 数字` の動画IDをファイル名から抽出）
- 再生時刻同期の弾幕オーバーレイ
- マウスドラッグで NG ユーザー登録（NG ドロップゾーン）
- NG ユーザー即時反映 + Undo（直近1件）
- 正規表現フィルタの登録/削除
- SQLite 永続化（NG/正規表現/コメントキャッシュ）

## 構成

- `app-ui/` : Qt 6 + QML + libmpv 埋め込み UI
- `core/` : Rust 製コア（取得、キャッシュ、フィルタ、永続化、JSON-RPC）
- `docs/` : 設計・プロトコル・テスト計画

## 開発要件

- Rust stable
- CMake 3.21+
- Qt 6.5+
- libmpv
- just（任意だが推奨）

## タスク実行（推奨）

`just` を使うと、Core/UI を横断するコマンドを共通化できます。

```bash
# 一覧
just

# Core テスト
just core-test

# 全体ビルド（core + ui）
just build

# 起動（NICONEON_CORE_BIN を自動設定）
just run
```

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
