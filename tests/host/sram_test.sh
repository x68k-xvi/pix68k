#!/bin/sh
# ホスト版の SRAM のファイルの扱いを確かめる。
#   1. 大きさの違う SRAM のファイルは読まずに無視する (SRAM を半端に書き換えない)
#   2. 書き戻しは一時ファイルから rename で置き換える (一時ファイルが残らず、16KB のファイルになる)
#   3. 2 回目の起動で、そのファイルを読む
#   4. ベアメタル版の形式 (16KB + 後書き) のファイルも読む
#   5. SRAM の $ED0008 からの 4 バイト (メインメモリの容量) は、既定では起動時に 4MB に直る。--sram-memsize keep では直さない
# 使い方: sram_test.sh <pix68k> 。ROM がなければ 77 (ctest の SKIP_RETURN_CODE)
set -eu
PIX68K=$1
ROM_DIR=${PIX68K_ROM_DIR:-$HOME/.cache/pix68k/rom}
[ -f "$ROM_DIR/IPLROM.DAT" ] || { echo "SKIP: no IPLROM.DAT"; exit 77; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy

head -c 100 /dev/zero > "$WORK/sram.dat"
"$PIX68K" --no-config -r "$ROM_DIR" --frames 120 --no-wait --sram "$WORK/sram.dat" > "$WORK/out1.txt" 2>&1
cat "$WORK/out1.txt"
grep -q "is not a valid SRAM file (100 bytes); ignored" "$WORK/out1.txt" || { echo "NG: short SRAM file was not rejected"; exit 1; }
# IPL ROM は SRAM を初期化するので、書き戻されて 16KB になる
size=$(wc -c < "$WORK/sram.dat" | tr -d ' ')
[ "$size" = 16384 ] || { echo "NG: SRAM file is $size bytes"; exit 1; }
if ls "$WORK" | grep -q tmp; then echo "NG: temporary file left"; ls "$WORK"; exit 1; fi

"$PIX68K" --no-config -r "$ROM_DIR" --frames 30 --no-wait --sram "$WORK/sram.dat" > "$WORK/out2.txt" 2>&1
cat "$WORK/out2.txt"
grep -q "^SRAM: $WORK/sram.dat" "$WORK/out2.txt" || { echo "NG: SRAM file was not read"; exit 1; }
# ベアメタル版の形式 (16KB + 後書き) も読める
python3 -c '
import struct, sys, zlib
data = open(sys.argv[1], "rb").read()
head = data + b"PX68SRAM" + struct.pack("<I", 3)
open(sys.argv[2], "wb").write(head + struct.pack("<I", zlib.crc32(head)))
' "$WORK/sram.dat" "$WORK/sram_bm.dat"
"$PIX68K" --no-config -r "$ROM_DIR" --frames 30 --no-wait --sram "$WORK/sram_bm.dat" > "$WORK/out3.txt" 2>&1
grep -q "^SRAM: $WORK/sram_bm.dat" "$WORK/out3.txt" || { cat "$WORK/out3.txt"; echo "NG: bare-metal SRAM file was not read"; exit 1; }
# SRAM の 8 バイト目からの 4 バイト (メインメモリの容量) を 16 進で読む / 書く
sram_mem() { python3 -c 'import sys; print(open(sys.argv[1], "rb").read()[8:12].hex())' "$1"; }
# set_sram_mem FILE HEX: FILE の 8 バイト目からの 4 バイトを HEX にする
set_sram_mem() { python3 -c 'import sys; f = open(sys.argv[1], "r+b"); f.seek(8); f.write(bytes.fromhex(sys.argv[2]))' "$1" "$2"; }
# check_mem 期待値 ログ 説明: SRAM のファイルの容量が期待値でなければ、ログを出して止める
check_mem() {
    got=$(sram_mem "$WORK/sram.dat")
    [ "$got" = "$1" ] && return 0
    cat "$2"
    echo "NG: $3 (memory size in SRAM: $got, want $1)"
    exit 1
}
# 最初の起動で IPL ROM が書いた SRAM は 4MB になっている
check_mem 00400000 "$WORK/out1.txt" "memory size was not set to 4MB"
# boot_sram LOG [オプション...]: sram.dat を使って 30 フレーム動かし、出力を LOG に書く
boot_sram() {
    log=$1
    shift
    "$PIX68K" -r "$ROM_DIR" "$@" --frames 30 --no-wait --sram "$WORK/sram.dat" > "$log" 2>&1
}
# 2MB にしておき、keep で起動しても 2MB のまま
set_sram_mem "$WORK/sram.dat" 00200000
boot_sram "$WORK/keep.txt" --sram-memsize keep
check_mem 00200000 "$WORK/keep.txt" "--sram-memsize keep rewrote the memory size"
# オプションなし (fix) で起動すると 4MB に戻る
boot_sram "$WORK/fix.txt"
check_mem 00400000 "$WORK/fix.txt" "memory size was not set back to 4MB"
echo ok
