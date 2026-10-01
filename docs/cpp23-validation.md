# C++23 移行の検証記録

2026-10-01 時点の #59 検証記録。移行の回帰テストと描画経路の qualification を分けて記載する。
最終 commit に対する CI と Windows 起動の確認は **pending**。
最終 Linux AppImage は SDK の loader/plugin/QML 環境変数を外した desktop 起動・自動終了が code 0。
直近のローカル build 成功を、最終 CI や runtime 全体の合格として扱わない。

## 検証環境

- Debian GNU/Linux 13.6 (x86-64)
- GCC 14.2.0、C++23、CMake 3.31.6、Ninja 1.12.1
- Qt 6.8.2、対応する GuiPrivate headers、QSQLITE
- libmpv 0.40.0（pkg-config の `mpv` API version は 2.5.0）
- Desktop OpenGL 4.5 Compatibility Profile、Mesa 25.0.7、llvmpipe (LLVM 19.1.7)
- clang-format / clang-tidy 19.1.7

llvmpipe は software renderer。以下の描画結果を実 GPU の性能・互換性に一般化しない。
依存と platform 別手順は [build-and-validation.md](build-and-validation.md) を参照。

## 自動テストと sanitizer

OpenGL integration を除く 16 CTest suites の結果:

| 条件 | 結果 | 範囲・制約 |
|---|---|---|
| Debug | 16/16 pass | 診断 hook 除去後のローカル build |
| Release | 16/16 pass | 同上 |
| ASan + UBSan | 16/16 pass | 同上。tracing 下の executor では `detect_leaks=0` |
| Desktop ASan + UBSan + LSan | 16/16 pass | 最終 cleaned source を desktop で再実行。`detect_leaks=1`、終了 code 0 |
| TSan | 一部 pass / 全体 inconclusive | domain/filter/timeline/store は pass。worker/network/controller は未解決の同期 report |

TSan の report は distribution の非 instrumented Qt/GLib `eventfd` 経路に現れ、
`QT_NO_GLIB=1` でも `QCoreApplication::postEvent` 周辺に現れた。
application race がないという証明にはならない。広い suppression は追加せず、
TSan-instrumented Qt を含む環境での調査を残す。
executor の LSan `ptrace` 制約と、実際に完走した desktop LSan は区別する。

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug -LE opengl
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release -LE opengl
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan
# tracing されていない desktop/CI で実行。preset は leak detection 有効
ctest --preset asan-ubsan -LE opengl
```

## RenderNode の pixel 回帰

独立した alignment test で、描画開始時の viewport が 100×100 のままになる問題を確認した。
renderer は `renderTarget()->pixelSize()` に基づき `glViewport` を設定し、
`ViewportState` を申告する。これは動的 GL state を仮定できないという
[Qt の render contract](https://doc.qt.io/qt-6.8/qsgrendernode.html#render) に対応する修正。
GuiPrivate はこの pixel-size 取得だけに使い、Qt 更新時は同じ build の headers と再検証が必要。

alignment fixture は実アプリ同様に背景 Rectangle を先行描画する構成に修正した。
Qt 6.8 の custom-node-only scene で無効な clip rectangle が生じる条件を避けつつ、
白い文字 pixel 数と平行移動の assertion は維持した。desktop で pixel assertion が pass。最終 cleaned binary でも再確認した。

## Video + overlay の厳密な検証と既知の制限

`video_overlay_e2e` は C++/QFile だけで Y4M を生成する。
fixture は 160×90、30fps、180 frames、Y=100 / U=V=128。
atlas / frame_image 用の両 fixture を別途 FFmpeg で decode し、先頭 frame の全 14,400 pixels が
RGB(97,97,97) であることを確認した。test 自体に FFmpeg や外部 asset の依存はない。

test は初回 window expose 前の load、再生時刻の進行、実際の白い glyph、overlay 共存中の背景、
hide 後の全背景、pause と seek (0 → 900 → 0ms) を確認する。
window manager による window resize に依存せず、実際の MpvItem 領域を crop する。
背景の各検査 pixel は RGB 98±8 を要求し、黒い grid を許容しない。OpenGL 不可時の skip は合格にしない。

同じ desktop で、各回を新規 process とし、3 条件を交互に実行した **full test** の結果:

| 条件 | 完全 pass / 実行数 |
|---|---|
| アプリ既定設定（`scale=lanczos`、継承済み `hwdec=auto-safe`） | 0/5 |
| `scale=bilinear` だけ変更 | 5/5 |
| `hwdec=no` だけ変更 | 0/5 |

失敗は overlay 作成前の mpv-only frame にも発生し、raw mpv FBO の RGB 自体が不正だった。
同じ capture の alpha は 255 で、fixture の decode 結果は正常だった。
原因の特定には至っておらず、**この環境の既定 video path は未合格**。
[mpv upstream #14577](https://github.com/mpv-player/mpv/issues/14577) は virtual X 上の黒画面・grid と
正常な overlay という関連症状の報告だが、同一原因と証明したものではない。

`NICONEON_MPV_SCALE=bilinear` は明示的な opt-in。初期化前に `scale` だけを変更し、受理結果を確認する。
Lanczos より柔らかい拡大出力になる。未設定・空・`default` は既定設定を維持し、
自動 fallback は行わない。上表の `hwdec=no` は診断用比較であり、採用する workaround ではない。

```sh
cmake --preset linux-debug -DNICONEON_BUILD_UI_E2E=ON
cmake --build --preset linux-debug
# 動作する OpenGL desktop で、両条件を独立して記録する
ctest --preset linux-debug -L opengl
NICONEON_MPV_SCALE=bilinear ctest --preset linux-debug -L opengl
NICONEON_MPV_SCALE=bilinear ./build/debug/niconeon
```

最終 cleaned binary でも alignment は pass、bilinear の full integration は新規 process 3/3 pass。
既定設定の再実行は atlas / frame_image 両方で失敗した。互換設定の pass を既定設定の pass に読み替えない。

## 残る gate

- 最終 source / commit の CI、Windows artifact の実起動は pending。最終 desktop sanitizer・pixel 再実行は上記の通り
- 実 GPU、Windows / native Wayland、DPR・高 refresh rate・HDR の qualification は pending
- 8h/24h soak、10k random seeks、frame-time / upload / memory の長時間測定は pending。60fps 達成の主張はしない
- GUI text raster と全 renderer queue/cache/cancellation/shutdown の残件は [#65](https://github.com/sh4869221b/niconeon/issues/65) / [#78](https://github.com/sh4869221b/niconeon/issues/78)
- 正式な順序・性能・配布 qualification は [#75](https://github.com/sh4869221b/niconeon/issues/75) と [test-plan.md](test-plan.md) に従う

この記録だけで後続 issue を close したり、release-ready と判定したりしない。
