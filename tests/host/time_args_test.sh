#!/bin/sh
# --time の値の検査 (pix68k と x68k_boot で共通。platform/time_arg.h)。ROM もディスクも要らない。
#   - 空、数でない、負、先頭の空白や + の付いた値、int64_t に入らない値 (上限 + 1 も) は受け付けず、終了コード 2 で終わる
#   - 値を省略すると、使い方を出して終了コード 2 で終わる
#   - 0 と int64_t の最大 (9223372036854775807) は受け付ける (RTC の範囲への丸めは Rtc::setTime() が行う)。
#     ROM の場所は空のディレクトリにするので、受け付けたあとは ROM がないとして終わる。そのときの終了コードと文
#     (pix68k はテストパターンを出して 0、x68k_boot は IPLROM.DAT を読めずに 1) を確かめる
# 使い方: time_args_test.sh <pix68k か x68k_boot> <受け付けたときの終了コード> <そのときの文 (grep -E)> <1 フレームで終わらせる引数...>
set -u
BIN=$1
OK_RC=$2
OK_TEXT=$3
shift 3
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
ng=0
for v in "" 12x -1 " 5" +5 9223372036854775808 99999999999999999999; do
    "$BIN" --no-config -r "$WORK" "$@" --time "$v" > "$WORK/out.txt" 2>&1
    rc=$?
    if [ $rc != 2 ] || ! grep -q -- "--time: want seconds" "$WORK/out.txt"; then
        cat "$WORK/out.txt"; echo "NG: --time '$v' was accepted (exit $rc)"; ng=1
    fi
done
"$BIN" --no-config -r "$WORK" "$@" --time > "$WORK/out.txt" 2>&1
rc=$?
if [ $rc != 2 ] || ! grep -q "^usage:" "$WORK/out.txt"; then cat "$WORK/out.txt"; echo "NG: --time without a value (exit $rc)"; ng=1; fi
for v in 0 9223372036854775807; do
    "$BIN" --no-config -r "$WORK" "$@" --time "$v" > "$WORK/out.txt" 2>&1
    rc=$?
    if [ $rc != "$OK_RC" ] || ! grep -qE -- "$OK_TEXT" "$WORK/out.txt" || grep -q -- "--time:" "$WORK/out.txt"; then
        cat "$WORK/out.txt"; echo "NG: --time $v: exit $rc (want exit $OK_RC and /$OK_TEXT/, no --time message)"; ng=1
    fi
done
[ $ng = 0 ] && echo "OK: --time values are checked"
exit $ng
