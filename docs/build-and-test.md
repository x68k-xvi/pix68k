# ビルドとテスト (ホスト)

ホスト版 (macOS / Linux、SDL2) のビルド、ctest と速さの測り方、オプション、入力、CI。

文中の #番号 は、開発用の非公開リポジトリの issue と PR の番号。

```sh
tools/fetch_sst.sh          # 68000 のテスト (SingleStepTests) を ~/.cache/pix68k/sst に取得・変換
cmake -S . -B build
cmake --build build -j
(cd build && ctest)         # SingleStepTests と、例外・割り込みなどの単体テスト
./build/bench               # ホスト上での CPU コアの速度 (68000)
./build/cpu020_bench        # 68030 / 68060 のコアだけの速さ (周辺機器なし。--cpu 68030 で 68030)
./build/machine_bench --profile   # X68000 全体 (CPU、周辺機器、画面と音の合成) の速さと内訳
./build/pix68k -0 disk.xdf  # ホスト版 (SDL2)。X68000 を実時間で動かす。IPL ROM と CGROM は ~/.cache/pix68k/rom から読む (-r で変える。
                            # IPL ROM がなければテストパターンを出す)。オプションの一覧は host/main.cpp の先頭、入力は下の「ホスト版の入力」
```

ホスト版には SDL2 が要る (`brew install sdl2`)。見つからなければホスト版だけビルドしない。

Release のビルドは `-fomit-frame-pointer` を付ける (#224。aarch64 の GCC で命令数が 1% ほど減る)。`perf record -g` などでフレームポインタを
たどって呼び出し元を見たいときは、`cmake -S . -B build -DPIX68K_FRAME_POINTER=ON` でビルドする。

Windows は MSYS2 の UCRT64 (`pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,SDL2}`) でビルドする。`C:\msys64\ucrt64\bin` を PATH に足して
次のようにすると `build-win/pix68k.exe` ができる (libgcc と libstdc++ は静的リンク。実行には SDL2.dll を exe の隣に置くか PATH に通す)。
RS-232C (`--rs232c`) は Windows では使えない。設定ファイルの既定は `%USERPROFILE%/.cache/pix68k/pix68k.cfg` (`--no-config` で使わない)。

```sh
cmake -S . -B build-win -G Ninja -DPIX68K_REQUIRE_SDL2=ON
cmake --build build-win --target pix68k
cp /c/msys64/ucrt64/bin/SDL2.dll build-win/
```

Linux (Ubuntu 26.04 の GCC 15 で確かめている) では、次のパッケージを入れると同じ手順でビルドできる。

```sh
sudo apt install build-essential cmake ninja-build libsdl2-dev python3
```

CI は `.github/workflows/ci.yml`。開発用の非公開リポジトリでは持ち主の機械の self-hosted runner で動かし、公開リポジトリ (j3tm0t0/pix68k) に
書き出すときに、`ci.yml` を GitHub のホストの runner の設定に差し替える。公開リポジトリの CI の job は 3 つある。
IPL ROM、CGROM、ディスクのイメージは置いていないので、それが要る試験は飛ばされる。

- `host-linux` (Ubuntu、GCC): ホスト版のビルドと ctest、`pico_kbd` のホストの試験
- `host-macos` (macOS、clang): ホスト版のビルドと ctest
- `baremetal` (Ubuntu、`aarch64-linux-gnu-gcc`): ベアメタル版 (QEMU 用の Raspberry Pi 3、Raspberry Pi 3 の 68000 だけの版、Raspberry Pi 3、Raspberry Pi 4、Raspberry Pi 5) のビルドと、
  ROM の要らない QEMU での起動の確認 (`tools/qemu_smoke.sh`)。ベアメタル版は `PREFIX64=aarch64-linux-gnu-` でビルドする

音 (OPM の FM 音源と ADPCM) はホスト版では SDL2 で 48kHz で出す。`--no-audio` で出さない。`--wav out.wav` で、出す代わりに WAV に書く
(`--no-wait` と組み合わせられる)。`x68k_boot` も `--wav` で WAV を書ける。
SDL2 で出すときは、ホストの処理が実時間に間に合わない間、ベアメタル版と同じく音を途切れさせずに遅く鳴らす (OPM と ADPCM は
低く鳴り、MIDI の音源は音の高さが変わらない。[baremetal.md](baremetal.md) の音の説明を参照)。WAV に書くときは、この調整をしない。
RTC の時刻は、既定ではホストの今の時刻から始まる。ゲームによっては RTC の値で動きが変わる (悪魔城ドラキュラは、起動した
時刻の秒によって約 10 秒からの音が変わる) ので、WAV や画面を前の版と比べるときは、`pix68k` も `x68k_boot` も
`--time 秒数` で時刻を決めておく。秒数は地方時の 1970-01-01 00:00:00 からの秒数として、時間帯を見ずにそのまま RTC に
入れる (`--time 1700000000` は、どの時間帯のホストでも 2023-11-14 22:13:20 になる)。
RTC が表せるのは 1980〜2079 年なので、その外の秒数は端 (1980-01-01 00:00:00 か 2079-12-31 23:59:59) にする。
`machine_bench` は IPL ROM と Human68k の起動ディスク (`~/.cache/pix68k`) で、起動、プロンプトのまま、計算のループ、
512x512 の画面とスプライトと OPM 8 チャネルを使う場面を動かし、実時間の何倍で動くか、最も重いフレームの時間、
場面の終わりの状態のチェックサム (タイミングを変えない変更の前後で同じになるはず) を出す。Pi では先頭の注釈の手順でビルドする。
Raspberry Pi 3 Model B+ の Linux (16MHz、画面と音の合成込み、1.2GHz と 1.4GHz が混じる) では、プロンプトのままと計算のループで実時間の約 2.5 倍、
gfx の場面 (512x512 で 4 ページ + スプライト 128 枚 + BG、OPM 8 チャネル) で約 1.08 倍。
`--render-thread` を付けると、画面の合成を別のスレッドで行う (ベアメタル版でコア 2 に置くのと同じしくみ。`src/x68k/video_replay.h`)。
`--sound-thread` を付けると、音の合成を別のスレッドで行う (コア 3 に置くのと同じしくみ。`src/x68k/sound_replay.h`)。
`--pin 1 2 -1` で 2 つのスレッドを別の CPU に固定して測ると、Raspberry Pi 3 Model B+ の Linux で gfx の場面が約 1.03 倍から約 1.76 倍に、
プロンプトのままで約 2.3 倍から約 3.1 倍になった (チェックサムは同じ。PR #27 の音の直流を切るフィルタなどを入れたあとの値)。
gfx の場面では合成するスレッドがほぼ休まず動いていて、Pi 3 で合成だけを受け持つコアの上限が実時間の約 1.8 倍になる。
`--sound-thread --pin 1 2 3` で音の合成も別の CPU に置くと、gfx の場面が約 1.83 倍、プロンプトのままで約 3.4 倍になった。
OPM の合成の速さは `./build/opm_bench` で測る (Raspberry Pi 3 Model B+ の Linux で、8 チャネルすべてを鳴らして 1 秒分に約 85ms、鳴っていなければ約 7ms)。

ホスト版の入力:

- キーボード: ホストのキーを物理的な位置で X68000 の JIS 配列のキーにする (`src/x68k/keyboard.cpp`)
- マウス: ウィンドウの中を左クリックすると、ホストのマウスを X68000 のマウスにする (カーソルが消え、ウィンドウから出なくなる)。
  中ボタンのクリックか、Ctrl と Alt (Mac は control と option) を同時に押すと元に戻る。フォーカスが外れたときも戻る
- ジョイスティック: SDL のゲームコントローラ (つないだ順にジョイスティック 1、2)。十字キーと左スティックが上下左右、A / X がトリガ A、B / Y がトリガ B。
  `--joy-keys` を付けると、カーソルキーと Z / X をジョイスティック 1 にする (そのキーは X68000 のキーボードには送らない。既定では送る)
- `--clock 10` で CPU を 10MHz (初代〜SUPER) にする。既定は 16MHz (XVI)。16MHz は 16.0MHz で動かす (実機の XVI は 16.67MHz)。
  68000 の機種では、VRAM、I/O、SRAM、ROM の読み書きのウェイトと、メインメモリの DRAM リフレッシュで遅れる分を入れている
  (値と出どころは `src/x68k/machine.cpp` の `setupBusWaits()` と `refreshWait()`)。TOMY さんの cpupower.r で測ると、
  10MHz で CPU / I/O / グラフィック VRAM / テキスト VRAM が 100.00% / 98.78% / 98.65% / 98.72% (実機の XVI 10MHz がすべて 100%)、
  16MHz で 160.67% / 106.58% / 143.14% / 148.08% (説明書の XVI 16.67MHz の値をクロックの比 16 / 16.67 で直すと 161.5% / 106.5% / 143.0% / 148.6%)
- 既定 (`--sram-memsize fix`) では、起動するときに SRAM のメインメモリの容量をエミュレータの容量 (`--ram`、既定 4MB) に書き換える。`--sram-memsize keep` なら SRAM の値を変えない
- キーボードの LED (CAPS、かな、INS など) は、点灯しているものをウィンドウのタイトルに出す

68000 の CPU コアは、SingleStepTests と食い違う所では、WinUAE (Amiga のエミュレータ) の動作か実機の計測値 (DIVS のオーバーフローのサイクル数など) に合わせている。
合わせた結果テストと合わないもの (ASR のフラグ、ADDQ.l #,An のサイクル数、除算のオーバーフロー・ゼロ除算の未定義フラグ、PC 相対オペランドのアドレスエラーの FC など) は
`tests/sst/expected_failures.txt` に理由付きで載せている。一覧は `tools/classify_sst.py` で作る。
