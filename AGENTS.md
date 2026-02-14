# AGENTS.md (Repository Guide)

このファイルは `niconeon` リポジトリで作業するエージェント向けの、リポジトリ固有ルールです。

## 1. 適用範囲と優先順位

- この `AGENTS.md` はリポジトリ全体に適用する。
- より深い階層に `AGENTS.md` / `AGENTS.override.md` がある場合はそちらを優先する。
- ユーザー向け説明は日本語を基本とする。

## 2. まず理解すべき前提

- UI: `app-ui/` (Qt 6 + QML + libmpv, C++)
- Core: `core/` (Rust)
- UI と Core は別プロセスで、`stdio` の NDJSON(JSON-RPC 2.0) で通信する。
- 責務境界を守ること:
  - 再生制御・描画・ドラッグ操作は UI 側。
  - コメント取得・キャッシュ・フィルタ・永続化は Core 側。

作業前に以下を確認すること:

- `README.md`
- `docs/architecture.md`
- `docs/protocol.md`
- `docs/test-plan.md`

## 3. 変更方針（重要）

- 依頼範囲を超えたリファクタや仕様変更をしない。
- 既存 API/挙動を変える場合は、ドキュメントとテストを同じ変更セットで更新する。
- UI と Core の結合を強めない。新機能は既存の JSON-RPC 契約に沿って拡張する。
- フィルタ順序は要件固定:
  - `NG user ID` を先に適用。
  - `regex` を後に適用。
- ドラッグ操作の要件を壊さない:
  - 掴んだコメントだけ停止。
  - NGドロップゾーンはドラッグ中のみ表示。
  - ゾーン外ドロップは復帰（同一レーン優先）。

## 4. プロトコル変更ルール

JSON-RPC のメソッド/パラメータ/レスポンスを変える場合は、必ず同時に更新すること:

- `core/crates/niconeon-protocol/src/lib.rs`
- `core/crates/niconeon-core/src/lib.rs`（実装）
- `app-ui/src/ipc/CoreClient.cpp` / `app-ui/src/ipc/CoreClient.hpp`（呼び出し）
- `docs/protocol.md`
- 関連テスト

互換性を壊す場合は、変更理由と移行方法を明記する。

## 5. コーディング規約

- 既存スタイルを優先し、差分は最小化する。
- Rust:
  - 失敗しうる処理は `Result` で返す。
  - 本番コードで安易な `unwrap()` を使わない（テストは可）。
  - 永続化や外部 I/O は `context` を付けてエラー原因を追跡可能にする。
- C++/Qt:
  - QObject の責務を明確にし、状態変化は signal で通知する。
  - UI スレッドで重い処理をしない。
- QML:
  - 見た目と軽量な操作ロジックを担当し、ドメインロジックは C++/Core に寄せる。
  - 座標系の違い（item/overlay/global）を明示的に扱う。

## 6. 検証手順

変更箇所に応じて最低限以下を実行する:

- Core変更時:
  - `just core-test` または `cd core && cargo test`
- UI変更時:
  - `cd app-ui && cmake -S . -B build`
  - `cd app-ui && cmake --build build -j`
- 横断変更時:
  - `just build`
  - 可能なら `just run` で起動確認

実行できなかった検証は、何を未実施かを必ず報告する。

## 7. テスト観点（特にUI）

UI 変更時は次を重点確認する:

- 再生/一時停止/シーク/音量の基本操作
- コメント同期（通常再生・一時停止・シーク後）
- ドラッグ中の他コメント挙動
- NGドロップ判定と即時反映（フェード）
- Undo（直近1件）
- 正規表現不正入力時のエラー表示

## 8. 依存関係とセキュリティ

- 新しい本番依存を追加する場合は、必要性と代替不能性を説明する。
- 秘密情報（トークン、Cookie、`.env`）をコミットしない。
- 外部コードを大量コピーしない。必要なら出典とライセンス整合を確認する。

## 9. Git運用

- コミットは小さく、目的単位で分ける。
- コミットメッセージは Conventional Commits 推奨（例: `fix(ui): ...`, `feat(core): ...`）。
- Issue を解決する変更を含むコミットでは、コミットメッセージに `Closes #<issue番号>`（または `Fixes #<issue番号>`）を必ず含め、GitHub で自動クローズされる形にする。
- 自分が作っていない変更は勝手に巻き戻さない。
- 破壊的コマンド（`git reset --hard` など）は明示指示なしで実行しない。
