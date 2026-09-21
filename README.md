# PlayStation homebrew sample

PSn00bSDK を使い、C のソースから初代 PlayStation 用の `hello.elf` と `hello.exe` を生成する macOS Apple Silicon 向けプロジェクトです。サンプルはダブルバッファーで文字と四角形を描画し、四角形を自動で動かします。PCSX-Redux の既定キーボード割り当てでは、矢印キーで位置を変えられます。

## 初回セットアップ

前提は Apple Silicon の macOS、Xcode Command Line Tools、Homebrew、Git です。セットアップは CMake、Ninja、公式 PCSX-Redux 定義の MIPS GCC/binutils、PSn00bSDK、PCSX-Redux、同梱 OpenBIOS を導入します。Homebrew の既存パッケージを一括更新せず、SDK とエミュレーターはプロジェクト内へ配置します。

```sh
./scripts/setup.sh
source scripts/env.sh
cmake --preset debug
cmake --build --preset debug
./scripts/run.sh
```

`setup.sh` は再実行できます。正常な固定版が導入済みなら、ツールチェーン、SDK、エミュレーターを再構築しません。ユーザーのシェル設定ファイルは変更しません。

PCSX-Redux の初回起動時に自動更新について尋ねられた場合は、好みに応じて `Enable auto update` または `No thanks` を選びます。公式 macOS 版は開発者証明書で署名されていないため、macOS が起動を止める場合は Finder で app を右クリックして「開く」を選ぶか、システム設定の「プライバシーとセキュリティ」から個別に許可します。

## 日常の操作

新しい zsh では、最初に環境を読み込みます。

```sh
source scripts/env.sh
```

Debug ビルド：

```sh
cmake --preset debug
cmake --build --preset debug
./scripts/run.sh
```

Release ビルド：

```sh
cmake --preset release
cmake --build --preset release
./scripts/run.sh build/release/hello.exe
```

`run.sh` は OpenBIOS、インタープリター CPU、内蔵デバッガーを有効にして EXE を直接ロードします。別のエミュレーターバイナリー、BIOS、個人データ配置を使う場合は、それぞれ `PCSX_REDUX`、`PCSX_REDUX_BIOS`、`PCSX_REDUX_DATA` を設定できます。

PCSX-Redux の既定設定では方向キーが D-pad に割り当てられています。変更する場合は Escape でメニューを表示し、`Configuration > Controls` を開きます。実行は F5、停止は F6 です。`run.sh` は Dynarec を無効にしてデバッガーを有効にするため、`Debug > Show Assembly` からブレークポイントと CPU 状態を確認できます。

## 生成物とディレクトリ

- `build/debug/hello.elf`: シンボルと DWARF デバッグ情報を保持する MIPS ELF。
- `build/debug/hello.exe`: PCSX-Redux へ直接渡す PS-X EXE。Windows 用 EXE ではありません。
- `build/release/`: Release 構成の同等生成物。
- `src/main.c`: 文字、ダブルバッファー描画、アニメーション、D-pad 入力のサンプル。
- `scripts/env.sh`: SDK、エミュレーター、PATH の zsh 用環境設定。
- `scripts/setup.sh`: 固定した依存関係の取得、検証、ビルド、配置。
- `scripts/run.sh`: 検証済み CLI オプションによるエミュレーター起動。
- `patches/`: PSn00bSDK v0.24 を macOS 26 / GCC 16 で構築するための限定パッチ。
- `.local/`: SDK、エミュレーター、ダウンロード、個人用エミュレーター設定。
- `third_party/`: 固定した PSn00bSDK ソースと submodule。
- `build/`: ビルド生成物と検証ログ。

採用版とチェックサムは [toolchain.lock](toolchain.lock)、実行済み検証は [docs/validation.md](docs/validation.md) に記録しています。

