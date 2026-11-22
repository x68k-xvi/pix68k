#!/bin/sh
# Circle の FatFs の設定 (addon/fatfs/ffconf.h) を pix68k 用に書き換える。
#   使い方: tools/ffconf_patch.sh <元の ffconf.h> <書き出す先>
# ベアメタル版のビルド (tools/build_baremetal.sh) と、ホストで FatFs の名前の扱いを確かめる試験 (fatfs_names_test) が使う。
#   FF_USE_FASTSEEK 1: 大きなディスクのイメージ (数百 MB) の中を読み書きするので、FAT をたどらずに位置を変えられる
#     fast seek を有効にする
#   FF_LFN_UNICODE 2、FF_CODE_PAGE 932: ファイル名を UTF-8 で受け渡し、長い名前のない古い 8.3 の名前は CP932 として
#     読む。元の 0 と 850 では、日本語の名前が設定メニューの一覧などで「?」になる (#275)
#   FF_LFN_BUF 765、FF_SFN_BUF 36: UTF-8 にすると名前のバイト数が増えるので、FILINFO の名前の場所を広げる。
#     長い名前は、FF_MAX_LFN の 255 字 (UTF-16) がすべて 3 バイトになっても入る 765 バイト (入らないと、FatFs は 8.3 の
#     短い名前を返す)。短い名前は、CP932 の 8.3 の 11 バイトが UTF-8 で 1 バイトにつき 3 バイトまで増えるので 36 バイト
#     (入らないと「?」になる)。FILINFO は 280 バイトから 812 バイトになる
set -eu
[ $# -eq 2 ] || { echo "usage: $0 <ffconf.h> <out>" >&2; exit 2; }
sed -e 's/^#define FF_USE_FASTSEEK[[:space:]]*0/#define FF_USE_FASTSEEK	1/' \
  -e 's/^#define FF_LFN_UNICODE[[:space:]]*0/#define FF_LFN_UNICODE	2/' \
  -e 's/^#define FF_CODE_PAGE[[:space:]]*850/#define FF_CODE_PAGE	932/' \
  -e 's/^#define FF_LFN_BUF[[:space:]]*255/#define FF_LFN_BUF		765/' \
  -e 's/^#define FF_SFN_BUF[[:space:]]*12/#define FF_SFN_BUF		36/' \
  "$1" > "$2"
for want in 'FF_USE_FASTSEEK[[:space:]]*1' 'FF_LFN_UNICODE[[:space:]]*2' 'FF_CODE_PAGE[[:space:]]*932' \
  'FF_LFN_BUF[[:space:]]*765' 'FF_SFN_BUF[[:space:]]*36'; do
  grep -q "^#define $want\$" "$2" || { echo "$want not set in $2" >&2; exit 1; }
done
