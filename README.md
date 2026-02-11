# Niconeon

Niconeon は、ローカル動画を再生しながらニコニココメントを時刻同期で弾幕表示する Windows / Linux 向けデスクトップアプリです。

## MVP 機能

- ローカル動画再生（再生/一時停止、シーク、音量）
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

## 制約

- コメント自動取得は、動画ファイル名にニコニコ動画ID（例: `sm9`, `so123456`）が含まれる前提です。
- ID 抽出に失敗した場合、動画再生は継続し、コメントは表示されません。
