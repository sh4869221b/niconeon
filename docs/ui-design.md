# UI Layout Design Document

## 1. Purpose

本書は Niconeon の **画面レイアウト設計** を定義する。
対象は `app-ui/qml/Main.qml` を中心とした主画面構成であり、
「どの UI 要素を、どこに、どの優先度で配置するか」を明確化する。

> 実装詳細（JSON-RPC スキーマ、Core 内部処理）は本書の対象外とし、必要時は関連文書を参照する。

## 2. Scope

- 対象画面
  - メイン画面（動画表示 + コメントオーバーレイ + 操作群）
  - 関連ダイアログ（Filter / Playback Speed / About）
  - 補助表示（Toast）
- 対象外
  - Core 側コメント取得アルゴリズム
  - 低レイヤ描画最適化の実装詳細

## 3. Layout Policy

### 3.1 Priority of Screen Real Estate

画面領域は以下の優先度で配分する。

1. **動画表示 + 弾幕表示領域（最優先）**
2. **再生操作の一次導線（上部ボタン群 + シーク/音量）**
3. **入力補助（ファイルパス入力）**
4. **二次操作（ダイアログ起動、計測、プロファイル切替）**
5. **一時通知（Toast）**

### 3.2 Base Window

- 既定ウィンドウサイズ: `1280 x 800`
- ルート: `ApplicationWindow`
- ルート直下の主レイアウト: `ColumnLayout`
- 全体 spacing: `8`

## 4. Main Screen Layout

## 4.1 Global Structure

主画面は `ColumnLayout` で以下を上から順に配置する。

1. **Top Action Row**（主要ボタン群）
2. **Path Row**（ファイル表示/入力）
3. **Transport Row**（シーク + 時間表示 + 音量）
4. **Player Area**（動画 + 弾幕）

---

### 4.2 Top Action Row（Row 1）

- レイアウト: `RowLayout`
- 配置特性
  - `Layout.fillWidth: true`
  - `Layout.margins: 8`
  - `spacing: 8`
- 配置順（左→右）
  1. 動画を開く
  2. 再生/一時停止
  3. 速度トグル（例: `1.0x`）
  4. 速度設定ダイアログ
  5. コメント表示/非表示
  6. 計測ログ開始/停止
  7. Profile 切替
  8. フィルタ
  9. About

**設計意図**
- 再生に必要な一次操作を 1 行に集約し、視線移動を最小化する。
- 状態を持つ操作（再生、コメント表示、計測ログ）はボタンラベルで現在状態を可視化する。

---

### 4.3 Path Row（Row 2）

- レイアウト: `RowLayout`
- 配置特性
  - `Layout.fillWidth: true`
  - `Layout.leftMargin/rightMargin: 8`
  - `spacing: 8`
- 構成
  - `Label("ファイル")`
  - `AppTextField`（`Layout.fillWidth: true`）

**設計意図**
- ファイル選択ダイアログを使わない手入力/貼り付け操作を許可する。
- テキストフィールドは横方向に最大限確保し、長いパスの視認性を高める。

---

### 4.4 Transport Row（Row 3）

- レイアウト: `RowLayout`
- 配置特性
  - `Layout.fillWidth: true`
  - `Layout.leftMargin/rightMargin: 8`
  - `spacing: 8`
- 構成（左→右）
  1. `seekSlider`（`Layout.fillWidth: true`）
  2. 再生位置/総時間ラベル（秒）
  3. `Vol` ラベル
  4. `volumeSlider`（`preferredWidth: 160`）

**設計意図**
- もっとも利用頻度の高いシークを横幅優先で配置する。
- 音量は固定幅で右端に配置し、画面幅変動時も操作位置を安定させる。

---

### 4.5 Player Area（Row 4, Main Content）

- コンテナ: `Item` (`id: playerArea`)
- 配置特性
  - `Layout.fillWidth: true`
  - `Layout.fillHeight: true`
  - `Layout.left/right/bottomMargin: 8`
  - `clip: true`
- 内部重なり順（下→上）
  1. 背景 `Rectangle`（黒、`radius: 8`）
  2. `MpvItem`（動画本体）
  3. `DanmakuOverlay`（コメント）

**設計意図**
- 動画と弾幕を同一領域で完全オーバーレイし、同期表示を単純化する。
- `clip: true` により表示領域外の描画を排除し、視覚破綻を防ぐ。

## 5. Overlay and Interaction Zones

### 5.1 Danmaku Overlay

- `DanmakuOverlay` は `playerArea` 全域に `anchors.fill` で配置する。
- `visible` は `commentsVisible` に連動し、コメント非表示時は描画負荷を抑える。

### 5.2 NG Drop Zone

- NG ドロップゾーンは **ドラッグ中のみ表示**。
- 通常時は画面を占有しない（誤操作防止）。
- ドロップ結果
  - ゾーン内: NG 登録フローへ遷移
  - ゾーン外: コメントを復帰（同一レーン優先）

## 6. Dialog Layout Positioning Policy

- `FilterDialog`
  - 主画面の文脈（NG/regex）を維持する補助画面
  - 開く前に最新フィルタ一覧を再取得する
- `PlaybackSpeedDialog`
  - 速度プリセット編集を担う
  - 主画面の速度ボタンと双方向整合する
- `AboutDialog`
  - ライセンス表示専用

**共通方針**
- 主画面の再生コンテキストを壊さないよう、短時間で閉じられる構成を維持する。

## 7. Toast Placement and Behavior

- 配置
  - `horizontalCenter: parent.horizontalCenter`
  - `bottom: parent.bottom`
  - `bottomMargin: 24`
- 表示時間: `3500ms`
- 役割
  - 操作結果の即時通知
  - NG Undo のアクション導線

## 8. Responsive Behavior

### 8.1 Width Shrink Strategy

- まず `Player Area` が縦方向に吸収し、動画領域を可能な限り維持する。
- 横幅不足時は Row 1 のボタン群が詰まるため、最小幅要件を下回る環境ではウィンドウ幅拡張を推奨する。
- Row 2/3 は `fillWidth` な入力・シークが優先的に幅を受け取る。

### 8.2 Height Shrink Strategy

- 4 行構成のうち、固定的な上部 3 行を保持し、`Player Area` が縮む。
- これにより最低限の操作導線を維持したまま動画表示を継続できる。

## 9. Layout Consistency Rules

1. 基本余白は `8px` を標準とする。
2. 同一行内の操作要素間隔は `8px` を維持する。
3. 再生関連の主要導線（再生/シーク/音量）は常時可視とする。
4. 動画領域は常に最下段の可変領域として確保する。
5. オーバーレイ要素は `playerArea` と同座標系で扱う。

## 10. Mapping to Current Implementation

本設計は以下実装に対応する。

- 主画面レイアウト
  - `ColumnLayout` + 3 つの `RowLayout` + `playerArea`
- プレイヤー重ね順
  - `Rectangle` → `MpvItem` → `DanmakuOverlay`
- 補助 UI
  - `FilterDialog`, `PlaybackSpeedDialog`, `AboutDialog`, `Toast`

## 11. Validation Checklist (Layout)

- [ ] Top Action Row のボタンが意図した順序で並ぶ
- [ ] Path Row の入力欄が横幅いっぱいに伸縮する
- [ ] Transport Row でシークバーが優先的に幅を確保する
- [ ] Player Area が残余領域を占有し、動画 + 弾幕が重なる
- [ ] コメント非表示時にオーバーレイが非表示化される
- [ ] ドラッグ中のみ NG ドロップゾーンが表示される
- [ ] Toast が画面下中央に表示される

## 12. Related Documents

- `README.md`
- `docs/architecture.md`
- `docs/protocol.md`
- `docs/test-plan.md`
