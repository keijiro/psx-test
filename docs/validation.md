# 検証記録

検証日：2026-09-16（Asia/Tokyo）

ホストは macOS 26.6.2 / arm64、Xcode 27.0、Apple Clang 21.0.0、Homebrew 6.0.22。固定した版とチェックサムは `toolchain.lock` を参照。

## セットアップとツールチェーン

`./scripts/setup.sh` を実行し、CMake 4.4.3、Ninja 1.13.2、binutils 2.47、GCC 16.2.0、PSn00bSDK v0.24、PCSX-Redux build 250 を導入した。2回目の実行は約8秒で終了し、正常な SDK とエミュレーターの再構築を省いた。

最小 C 関数を次の条件でコンパイルした。

```sh
printf '%s\n' 'int add(int a, int b) { return a + b; }' |
  mipsel-none-elf-gcc -march=r3000 -mabi=32 -EL -x c -c -o build/toolchain-check/probe.o -
mipsel-none-elf-objdump -f build/toolchain-check/probe.o
```

結果は `elf32-littlemips`、`architecture: mips:3000`。`file` は `ELF 32-bit LSB relocatable, MIPS-I` と判定した。コンパイラー、アセンブラー、リンカー、objdump は全て起動した。

PSn00bSDK v0.24 は Apple Clang 21 と GCC 16 に対して3点の限定修正を要した。

- ホスト用 LZP 圧縮コードで `stdlib.h` を常に読み込む。
- 描画キューの関数ポインターを実装どおり3引数型にする。
- macOS の `mkpsxiso` で `stat64` を `stat` に写像する。

修正は `patches/psn00bsdk-v0.24-macos-clang.patch` に保存した。警告やエラーの広域抑制は使っていない。SDK の Debug/Release ライブラリ、`elf2x`、`mkpsxiso` を含むホストツールをビルドし、`.local/psn00bsdk` にインストールした。

SDK 付属 beginner/hello も別途ビルドし、MIPS-I ELF と `PS-X EXE` シグネチャを確認した。

## プロジェクトのビルド

新しい zsh から公開手順を実行した。

```sh
source scripts/env.sh
cmake --preset debug
cmake --build --preset debug
cmake --preset release
cmake --build --preset release
```

結果：

| 生成物 | 判定 | SHA-256 |
| --- | --- | --- |
| `build/debug/hello.elf` | MIPS-I LSB ELF、デバッグ情報・シンボルあり | `25bdfa29b042fef7da69afd0cd85a1af56b8e3d9a3a2afb8da5461a8de048c2e` |
| `build/debug/hello.exe` | Sony PlayStation executable、`PS-X EXE` | `2f7a01440be49b3f89f7a44d2b570b48ca0b74511fc6c6470e1945b573775904` |
| `build/release/hello.elf` | MIPS-I LSB ELF | `f54c7d1492d8e12d68f603dd80af396d336e90058ce243bb31fa3b803158d0da` |
| `build/release/hello.exe` | Sony PlayStation executable、`PS-X EXE` | `8c36aa4934675c5012f9e83837cf4c6edb5279989b7e5125e7e83d571e71f1eb` |

最終ソースを新規の `build/final-clean-20260916` に configure/build し、既存中間生成物なしでも同じ PS-X EXE を生成した。

## エミュレーターと画面

公式 AppDistrib の macOS ARM build 250 を使用した。バンドル内の `openbios.bin` を PCSX-Redux が `OpenBIOS detected (0b0359a7)` と認識し、`hello.exe` を直接ロードした。検証ログは `build/validation/pcsx-debug.log` にある。

画面で次を確認した。

- 濃紺背景に `PSN00BSDK VALIDATED` と `D-PAD / ARROW KEYS: MOVE` が表示された。
- 黄色の四角形がフレームごとに水平方向へ移動し、色も時間変化した。メニュー表示時の計測は約59.9 FPSだった。
- 20回の上矢印入力により、四角形の上端が画面上で約125ピクセル上へ移動した。
- 背景色と先頭文字列を変更し、再ビルド・再起動後に新しい濃紺色と `VALIDATED` 表示へ変わった。

PCSX-Redux が保存した設定でも `Dynarec: false`、`Debug.Debug: true`、方向キーの SDL scancode（Left 80、Right 79、Up 82、Down 81）を確認した。

## デバッガー

Debug ELF のシンボルから `main = 0x80010474` を取得した。PCSX-Redux を `-interpreter -debugger` で起動し、Lua API で同アドレスに実行ブレークポイントを設定した。

ログには次が記録された。

```text
Breakpoint triggered: PC=0x80010474 - Cause: 80010474::Exec::4 (Lua Breakpoint)
```

Assembly 画面で PC の黄色矢印が `RAM:80010474` を指し、そのメモリー上の命令ワードが `27bdffd8`（`addiu sp,sp,-40`）であることを確認した。F5 で再開すると PC 矢印が消え、その後 GPU とパッドの初期化ログが継続した。

