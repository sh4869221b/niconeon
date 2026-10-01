# Niconeon

Niconeon は、ローカル動画を再生しながらニコニココメントを時刻同期で弾幕表示する Windows / Linux 向けデスクトップアプリです。

## 構成

C++23 / Qt 6 の **単一プロセス・単一アプリケーション `niconeon`** です。
QML はレイアウト、見た目、操作の通知を担当し、再生制御・設定・コメント取得・SQLite・フィルタ・コメント放出は C++ が所有します。

- `src/app/`: ApplicationController、非同期 CommentService、エントリーポイント
- `src/domain/`, `src/comments/`, `src/filters/`, `src/storage/`: 型付きモデル、取得、timeline、NG/Undo、SQLite
- `src/playback/`, `src/danmaku/`: libmpv、既存 sprite/atlas renderer
- `src/ui/qml/`: View と presentation-only components
- `tests/`, `cmake/`, `packaging/`: Qt Test / Quick Test、CMake、単一アプリ配布

Rust、Cargo、Bazel、独立 core executable、stdio JSON-RPC は使用しません。

## 主な機能

- ローカル動画再生、Pause/Resume、Seek、音量、速度プリセット
- ファイル名の `sm` / `nm` / `so` + 数字からコメント取得
- NGユーザーのドラッグ登録、即時フェード、直近1件のUndo
- 正規表現フィルタ、SQLite永続化、取得失敗時のキャッシュfallback
- コメント表示切替、runtime profile (`high` / `balanced` / `low_spec`)、放出上限・同一内容coalesce
- 計測ログ、About / ライセンス表示

IDなし・コメント取得失敗・キャッシュ不正でも、有効なローカル動画の再生は継続します。
初回ロードは描画contextの準備完了まで保持し、手動で開き直す必要がない構成です。

## 開発

採用環境は C++23 対応 compiler、CMake 3.25+、Ninja、Qt 6.8+、同一 Qt build の GuiPrivate 開発ヘッダー（Debian: `qt6-base-private-dev`）、libmpv、pkg-config、Qt SQL SQLite driver です。GuiPrivate は renderer の render-target pixel size 取得だけに使い、Qt minor 更新時には rebuild と OpenGL 回帰テストが必要です。Windows UCRT64 は `qt6-base` package に同ヘッダーを含みます。
Linux はディストリビューションの依存を使い、Windows は MSYS2 UCRT64 を使用します。

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
./build/debug/niconeon

cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
```

Windows、Sanitizer、clang-format / clang-tidy、パッケージングの詳細は [build-and-validation](docs/build-and-validation.md) を参照してください。
`CMAKE_EXPORT_COMPILE_COMMANDS=ON` が既定で有効です。

## 設定・データ互換性

既存のNG/regex DBと分離済みコメントcache DBのパス・schemaを維持します。
旧データDB内 `comment_cache` は、cache DBへのtransaction commit後に移行元tableを削除します。移行失敗時は元データを保持します。

- Linux: `$XDG_DATA_HOME/niconeon/niconeon.db` / `$XDG_CACHE_HOME/niconeon/comment-cache.db`（未設定時は標準HOME配下）
- Windows: `%APPDATA%/sh4869221b/niconeon/data/niconeon.db` / `%LOCALAPPDATA%/sh4869221b/niconeon/cache/comment-cache.db`
- UI設定は従来のQt organization/application (`sh4869221b` / `Niconeon`) を維持
- regex は `QRegularExpression` (PCRE2) に移行。一般的な既存patternとUnicodeは試験し、無効な保存patternを黙って破棄しません。Rust regex固有の構文との完全互換は保証しません

## 計測・再現

```sh
NICONEON_AUTO_VIDEO_PATH=/path/to/movie_sm9.mp4 \
NICONEON_SYNTHETIC_COMMENTS=ramp NICONEON_AUTO_PERF_LOG=1 \
NICONEON_AUTO_EXIT_MS=60000 ./build/release/niconeon
```

描画targetは既定60fpsです。`NICONEON_DANMAKU_RENDERER=atlas|frame_image`、`NICONEON_DANMAKU_WORKER=on|off`、`NICONEON_SIMD_MODE=auto|scalar|avx2` を維持しています。
60fpsの正式達成や実GPU性能はビルド成功・headless testから推定しません。

## 移行と後続gate

この変更は [#59](https://github.com/sh4869221b/niconeon/issues/59) の移行です。
新規network/JSON/SQLite/filter処理は専有workerで実行し、queue上限・generation cancellation・停止処理を実装しています。
既存rendererのGUI raster・cache/queue上限を含む全runtime qualificationは [#65](https://github.com/sh4869221b/niconeon/issues/65) / [#78](https://github.com/sh4869221b/niconeon/issues/78) のblocking follow-upです。
media clock/cursor、production video/text方式、60fps性能、Windows実GPU、Wayland、HDR、長時間安定性の正式gateは [#75](https://github.com/sh4869221b/niconeon/issues/75) に従います。

[Architecture / ownership](docs/architecture.md) と [test plan](docs/test-plan.md) に検証範囲と残件を記載しています。

## ライセンス

自作ソースはMIT (`LICENSE`)、配布バイナリはGPL-3.0-or-later (`COPYING`) 条件です。
配布物には `LICENSE` / `COPYING` / `SOURCE_CODE.md` / `THIRD_PARTY_NOTICES.txt` を同梱します。
実際に同梱するQt/libmpvと推移依存のライセンス・対応ソース義務はリリースごとに確認してください。
