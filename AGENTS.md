# Repository guide

ユーザー向け説明は日本語を基本とする。このガイドはリポジトリ全体に適用する。
作業前に README.md、docs/architecture.md、docs/application-contracts.md、docs/test-plan.md を読む。

## Architecture invariants

- C++23 / Qt 6 single-process application。CMake + Ninjaだけを正式build入口とする
- QMLはViewのみ。network、JSON、DB、timeline、filter、player state、QoSはC++に置く
- GUI / render threadへnetwork・JSON・SQL処理を持ち込まない
- Cross-thread ownership、queue上限、overflow、generation、shutdown契約をdocsとtestsで維持する
- 既存rendererのraster/queue残件は#65/#78。移行をruntime最終qualification済みと扱わない
- NG user IDを先に、regexを後に適用する
- 掴んだコメントのみ停止、NG zoneはdrag中のみ、外dropは同一lane優先で復帰、失敗したNG登録はfadeをrollbackする
- 既存DB/cache schemaとplatform path互換性を保つ。変更時は移行とtestsも同時更新

## Validation

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
cmake --build build/debug --target format-check
```

変更に応じてRelease、ASan/UBSan、TSan、clang-tidy、QML、OpenGL integrationを実行する。
実行不能・skip・未検証はpassと区別する。画面上のコメント可視性はactive countだけで判断しない。
詳細はdocs/build-and-validation.mdを参照。

## Change and Git policy

- 依頼範囲外の仕様変更・リファクタをしない。変更するAPI/挙動にはdocs/testsを付ける
- QObjectの所有者とthreadを明確にし、状態変化はtyped signals/slotsで通知する
- 自分が作っていない変更は巻き戻さない。破壊的Git操作は明示指示なしで行わない
- 目的単位の小さなConventional Commit、英語commit messageを推奨する
- Issue全体を実際に解決したcommitにのみCloses/Fixesを付ける。部分実装やqualification未完了のdraftにはRefsを使う
- Draft PR作成はmerge・tag・release公開の許可ではない
- credentials、cookies、private local paths、build directory、toolchain binariesをcommitしない
- 外部コードの大量copyや依存追加には必要性・出典・licenseを確認する
