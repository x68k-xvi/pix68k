#!/bin/sh
# ホスト版の --time (RTC の時刻を決める) を確かめる。
#   1. Human68k の time コマンドが、--time で決めた時刻 (22:13 と、1 時間あとの 23:13) を出す
#   2. 同じ --time で 2 回走らせると、画面 (PPM) も音 (WAV) も同じになる
# 値の検査は ROM なしで動く time_args_test.sh で確かめる。
# 既定ではホストの今の時刻から始まるので、時刻で動きの変わるゲーム (悪魔城ドラキュラなど) は、実行ごとに WAV が変わる (#149)。
# 使い方: time_test.sh <pix68k> <Human68k の起動ディスク>。ROM (IPLROM.DAT と CGROM.DAT) かディスクがなければ 77 (ctest の SKIP_RETURN_CODE)
set -eu
PIX68K=$1
DISK=$2
ROM_DIR=${PIX68K_ROM_DIR:-$HOME/.cache/pix68k/rom}
[ -f "$ROM_DIR/IPLROM.DAT" ] || { echo "SKIP: no IPLROM.DAT"; exit 77; }
# 画面の字は CGROM の字形と比べて読む (--dump-text) ので、CGROM がなければ時刻を確かめられない
[ -f "$ROM_DIR/CGROM.DAT" ] || { echo "SKIP: no CGROM.DAT"; exit 77; }
[ -f "$DISK" ] || { echo "SKIP: no $DISK"; exit 77; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy

run() {
    name=$1; shift
    "$PIX68K" --no-config -r "$ROM_DIR" --no-sram --read-only --no-wait -0 "$DISK" --frames 1300 --type 'time\n' --type-after 900 \
        --screenshot "$WORK/$name.ppm" --wav "$WORK/$name.wav" --dump-text "$@" > "$WORK/$name.txt" 2>&1
}
# 1700000000 は、ホストの時間帯によらず 2023-11-14 22:13:20 (時間帯を見ずに地方時として RTC に入れる)
run a --time 1700000000 &
pa=$!
run b --time 1700000000 &
pb=$!
run c --time 1700003600 &
pc=$!
st=0
wait $pa || st=1
wait $pb || st=1
wait $pc || st=1
[ $st = 0 ] || { cat "$WORK/a.txt" "$WORK/b.txt" "$WORK/c.txt"; echo "NG: pix68k failed"; exit 1; }
for f in a.ppm b.ppm a.wav b.wav; do [ -s "$WORK/$f" ] || { echo "NG: $f was not written"; exit 1; }; done
# time コマンドの出す時刻 (日本語の部分は読めないので ? になる)。起動して約 16 秒後に打つので 22:13:36 前後になる
# 時が 1 桁のとき (" 0:49:11") も拾う
show() { grep -a "^|" "$WORK/$1.txt" | grep -oE '[0-9]{1,2}:[0-9]{2}:[0-9]{2}' | head -1; }
for n in a b c; do echo "$n: $(show $n)"; done
for n in a b; do
    case "$(show $n)" in 22:13:*) ;; *) grep -a "^|" "$WORK/$n.txt"; echo "NG: --time 1700000000 did not show 22:13"; exit 1 ;; esac
done
case "$(show c)" in 23:13:*) ;; *) grep -a "^|" "$WORK/c.txt"; echo "NG: --time 1700003600 did not show 23:13"; exit 1 ;; esac
cmp -s "$WORK/a.ppm" "$WORK/b.ppm" || { echo "NG: same --time gave different screens"; exit 1; }
cmp -s "$WORK/a.wav" "$WORK/b.wav" || { echo "NG: same --time gave different WAV files"; exit 1; }
echo "OK: --time sets the clock, and the same --time gives the same screen and WAV"
