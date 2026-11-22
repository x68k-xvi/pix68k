# RetroPie: lr-pix68k

既存の `lr-px68k` と別のコアとして X68000 に登録する。68000 / 2MB / OPM・ADPCM が既定。
CMake 3.20 以上と 64bit Linux が必要 (共有 CPU ライブラリの `__int128` のため)。32bit Pi OS と Vita は対象外。

## 導入

RetroPie-Setup のある環境で、このファイルと同じディレクトリのモジュールをコピーする。

```sh
cp retropie/lr-pix68k.sh ~/RetroPie-Setup/scriptmodules/libretrocores/
cd ~/RetroPie-Setup
for action in depends sources build install configure; do
    sudo ./retropie_packages.sh lr-pix68k "$action" || break
done
```

公開リポジトリの main 以外 (手元で直したものなど) を使うときは、`sources` の代わりにそのチェックアウトを
`RetroPie-Setup/tmp/build/lr-pix68k` に置いて、`build` から実行する。
ビルド生成物は `build-libretro/pix68k_libretro.so`、導入先は
`/opt/retropie/libretrocores/lr-pix68k/`。
RetroArch、runcommand、EmulationStation は RetroPie-Setup で別途導入しておく。

## BIOS・媒体・操作

- `~/RetroPie/BIOS/pix68k/IPLROM.DAT`: 128KB。`CGROM.DAT`: 768KB。
- `~/RetroPie/roms/x68000/`: XDF / DIM / D88 / M3U / SASI HDF。
- EmulationStation の X68000 から選ぶ。既存の既定エミュレータは変更しない。
  `lr-px68k` が既定なら、runcommand の起動メニューで `lr-pix68k` を選ぶ。
- Core Options の `Disk Control target drive` と RetroArch のディスク操作で FD0 / FD1 を交換する。
- SRAM・ステート・ディスクの保存先は RetroArch の設定に従う。ディスクは既定で書込み禁止。
  `save-copy` では save directory の `pix68k-media/` だけに書き、元イメージは変更しない。
  標準設定では ROM フォルダが save directory になる。コピーは `.pxm` で保存し、ゲーム一覧に出さない。
  保存を別にまとめる場合は RetroArch の `savefile_directory` を設定する。
- `pix68k_libretro.info` も RetroArch のコア情報ディレクトリへコピーする。
  RetroArch の情報ファイル一式を更新して消えた場合は、モジュールの `configure` を再実行する。
- キーボードで Human68k を操作するときは Scroll Lock で RetroArch のゲームフォーカスを有効にする。
- 巻戻し・runahead・netplay は未検証。情報ファイルには通常のステート対応 (`basic`) を指定する。

Pi 5 の HDMI で ALSA の `hw:` に S16_LE のエラーが出る場合、音声デバイスを
`plughw:CARD=vc4hdmi0,DEV=0` など対応する HDMI 出力に指定する。
コア・情報ディレクトリは RetroArch の `libretro_directory` / `libretro_info_path` で指定できる。

実機の検証条件と結果は [docs/compat.md](../docs/compat.md) に記録する。
