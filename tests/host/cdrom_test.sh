#!/bin/sh
# ホスト版で、CUE シートの CD-ROM のイメージをつなぎ、設定メニューで媒体を入れ替える。
#   1. --cd に CUE シートを渡すと、BIN を読んでトラックの数を出す
#   2. メニューの CD-ROM で別のイメージ (ISO) を選ぶと、動かしたまま入れ替える
#   3. (none) を選ぶと取り出す
#   4. 読めない CUE シート (BIN がない) を選ぶと、入っている媒体はそのまま
# CD-ROM は SCSI につなぐので、X68030 (内蔵 SCSI) で動かす。
# 使い方: cdrom_test.sh <pix68k> <IPLROM30.DAT> 。ROM がなければ 77 (ctest の SKIP_RETURN_CODE)
set -eu
PIX68K=$1
IPL30=$2
ROM_DIR=${PIX68K_ROM_DIR:-$HOME/.cache/pix68k/rom}
[ -f "$ROM_DIR/CGROM.DAT" ] || { echo "SKIP: no CGROM.DAT"; exit 77; }
[ -f "$IPL30" ] || { echo "SKIP: no IPLROM30.DAT"; exit 77; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
cd "$WORK"
mkdir images
# 300 セクタ (2352 バイト / セクタ) の BIN: トラック 1 はデータ (150 セクタ)、トラック 2 はオーディオ (150 セクタ)
head -c 705600 /dev/zero > images/TEST.BIN
printf 'FILE "TEST.BIN" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 00:02:00\n' > images/TEST.CUE
head -c 131072 /dev/zero > images/GAME2.ISO
printf 'FILE "NONE.BIN" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n' > images/BROKEN.CUE
run() {
  "$PIX68K" --no-config -r "$ROM_DIR" --cpu 68030 --ipl "$IPL30" --image-dir images --cd images/TEST.CUE --no-sram --no-wait \
    --frames "$1" --press-after 60 --press "$2" > out.txt 2>&1 || { cat out.txt; echo "NG: pix68k failed"; exit 1; }
  cat out.txt
}
want() { grep -qF -- "$1" out.txt || { echo "NG: $1"; exit 1; }; }

# X68030 の項目: FD0 FD1 HD0 CD-ROM ...。CD-ROM の一覧は (none)、BROKEN.CUE、GAME2.ISO、TEST.CUE
run 200 menu,down,down,down,enter,home,down,down,enter,esc
want 'images/TEST.CUE (300 blocks, 2 tracks, 1 audio), CD-ROM'
want 'menu: CD-ROM: images/GAME2.ISO'
# 取り出す
run 200 menu,down,down,down,enter,home,enter,esc
want 'menu: CD-ROM: ejected'
# 読めない CUE シートは入れない
run 200 menu,down,down,down,enter,home,down,enter,esc
want 'menu: CD-ROM: images/BROKEN.CUE: cannot read images/NONE.BIN'
grep -qF 'menu: CD-ROM: ejected' out.txt && { echo "NG: the medium was removed"; exit 1; }
echo OK
