#!/bin/sh
# ベアメタル版をビルドする。
#   使い方: tools/build_baremetal.sh <3|4|5> [--qemu]
#   PIX68K_CPU020=0 を付けると 68030 / 68060 のコアを入れない (68000 だけ)。既定は 1 (入れる)
#   出力: build/baremetal-raspi<N>[-qemu]/app/kernel8.img (Pi 3) / kernel8-rpi4.img (Pi 4、Pi 400) / kernel_2712.img (Pi 5)
# Circle はビルド先ごとに設定が違うので、external/circle をビルドディレクトリに写してからビルドする。
set -eu
usage() { echo "usage: $0 <3|4|5> [--qemu]" >&2; exit 2; }
[ $# -ge 1 ] && [ $# -le 2 ] || usage
N=$1
case $N in 3|4|5) ;; *) usage ;; esac
QEMU=
if [ $# -eq 2 ]; then
  [ "$2" = --qemu ] || usage
  [ "$N" = 3 ] || { echo "QEMU は raspi3b だけ対応している" >&2; exit 2; }
  QEMU=--qemu
fi
PREFIX=${PREFIX64:-aarch64-elf-}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/build/baremetal-raspi$N${QEMU:+-qemu}
mkdir -p "$OUT"
# 前のビルドのイメージを先に消す。途中のどこで失敗しても (Circle の設定、ライブラリ、アプリ、最後の _end の検査)、
# 古いイメージが残って、誤って SD カードに写すことがないようにする
rm -f "$OUT"/app/kernel*.img

# ソースは --delete で揃える (submodule の更新で消えたファイルを残さない)。
# ビルドの生成物は除外するので消えない。external/circle の中で直接ビルドしたときの生成物も写さない
rsync -a --delete --exclude .git \
  --exclude '*.o' --exclude '*.a' --exclude '*.d' --exclude '*.elf' --exclude '*.img' --exclude '*.map' \
  --exclude '*.lst' --exclude Config.mk --exclude .pix68k-config --exclude /addon/fatfs/ffconf.h \
  --exclude /include/circle/timer.h --exclude /lib/usb/dwhcidevice.cpp --exclude /lib/net/udpconnection.cpp \
  "$ROOT/external/circle/" "$OUT/circle/"
cd "$OUT/circle"
# FatFs の設定は tools/ffconf_patch.sh で書き換える (fast seek、UTF-8 のファイル名など。理由はそちらに書いた)。
# submodule は変えず、写した先だけを書き換える (中身が変わったときだけ置き換え、毎回 FatFs を作り直さないようにする)
FFCONF=addon/fatfs/ffconf.h
sh "$ROOT/tools/ffconf_patch.sh" "$ROOT/external/circle/$FFCONF" "$FFCONF.tmp"
if cmp -s "$FFCONF.tmp" "$FFCONF"; then rm "$FFCONF.tmp"; else mv "$FFCONF.tmp" "$FFCONF"; fi
# CTimer::MsDelay(): pix68k_msdelay_hook (baremetal/kernel.cpp) が nullptr でなければ、それを呼ぶ (ほかのコアが読む間に
# コア 0 が nullptr に戻すことがあるので、1 回だけ読む)。
# USB の装置を探している間 (ハブの電源を入れて待つなど、待ち時間のほとんどは MsDelay()) に、コア 0 が画面を写し、
# 大きなイメージのチャンクを読めるようにする (kernel.cpp の ScanUsb())。ffconf.h と同じく、写した先だけを書き換える
TIMERH=include/circle/timer.h
awk -v old='{ SimpleMsDelay (nMilliSeconds); }' \
    -v new='{ void (*h) (unsigned) = pix68k_msdelay_hook; if (h) h (nMilliSeconds); else SimpleMsDelay (nMilliSeconds); }' '
  /^class CTimer/ && !d { print "extern \"C\" void (*volatile pix68k_msdelay_hook) (unsigned nMilliSeconds);"; d = 1 }
  index($0, "void MsDelay (unsigned nMilliSeconds)") && (i = index($0, old)) {
    $0 = substr($0, 1, i - 1) new substr($0, i + length(old)); m = 1
  }
  { print }
  END { if (!d || !m) exit 1 }' "$ROOT/external/circle/$TIMERH" > "$TIMERH.tmp" ||
  { echo "CTimer::MsDelay() not found in $TIMERH" >&2; exit 1; }
if cmp -s "$TIMERH.tmp" "$TIMERH"; then rm "$TIMERH.tmp"; else mv "$TIMERH.tmp" "$TIMERH"; fi
# QEMU 用のビルドだけ、DWHCI (Pi 3 の USB ホストコントローラ) のドライバの、転送が終わったときの割り込みの状態から、
# 転送が終わっている (XFER_COMPLETE) ときは NAK と NYET を外す。QEMU の DWC2 (hw/usb/hcd-dwc2.c) は、bulk の IN に
# 装置が NAK を返すと、HCINT に NAK を立てたまま自分で送り直し、データが届いて終わったときも NAK を消さない。
# Circle はその NAK を見て「データがない」と扱い、受け取った長さを 0 にするので、usb-net のフレームが消える
# (Mac の QEMU 11.1 で、USE_NAK_USB_FIX がないと DHCP の答えを受け取れなかったのはこのため)。
# 前は Circle の USE_NAK_USB_FIX (NAK で転送を終え、チャネルを止める) で避けていたが、QEMU はチャネルを止めても
# (HCCHAR の CHDIS) 送り直しをやめない。止めたチャネルがフレームを受け取り、Circle が次の転送でそれを上書きするので、
# HTTP の SYN などがほとんど届かなかった (#117。x86_64 の Linux の QEMU 10.2 の -trace で確かめた)。
# 実機のビルドは写すだけで、書き換えない (実機では確かめていない)
DWHCI=lib/usb/dwhcidevice.cpp
if [ -n "$QEMU" ]; then
  awk -v old='pStageData->TransactionComplete (ChanInterrupt.Read (),' \
      -v new='pStageData->TransactionComplete ((ChanInterrupt.Read () & DWHCI_HOST_CHAN_INT_XFER_COMPLETE) ? (ChanInterrupt.Get () & ~(DWHCI_HOST_CHAN_INT_NAK | DWHCI_HOST_CHAN_INT_NYET)) : ChanInterrupt.Get (),' '
    (i = index($0, old)) { $0 = substr($0, 1, i - 1) new substr($0, i + length(old)); m++ }
    { print }
    END { if (m != 1) exit 1 }' "$ROOT/external/circle/$DWHCI" > "$DWHCI.tmp" ||
    { echo "TransactionComplete (ChanInterrupt.Read (), ... not found once in $DWHCI" >&2; exit 1; }
else
  cp "$ROOT/external/circle/$DWHCI" "$DWHCI.tmp"
fi
if cmp -s "$DWHCI.tmp" "$DWHCI"; then rm "$DWHCI.tmp"; else mv "$DWHCI.tmp" "$DWHCI"; fi
# QEMU 用のビルドだけ、閉じた UDP の接続 (CUDPConnection) は、届いたパケットを受け取らない (PacketReceived() の頭で
# 0 を返し、一覧の次の接続に回す)。Circle の DHCP (lib/net/dhcpclient.cpp) は Bind(68) で作った接続を Connect() の中で
# 閉じて作り直すが、閉じた接続が一覧から消えるのは次の CTransportLayer::Process() のとき。それより先に OFFER が届くと、
# 一覧で前にある閉じた接続が OFFER を「受け取った」と返して捨て、4 秒後の送り直しまでアドレスが決まらなかった
# (#136。QEMU の slirp は OFFER をすぐ返し、#117 の直しで受信の転送がいつも待ち受けるので、x86_64 の Linux の QEMU 10.2 でも
# Mac の QEMU 11.1 でも毎回起きた)。
# 実機でも DHCP サーバの答えが速ければ起きうるが、実機では確かめていないので、実機のビルドは写すだけで書き換えない
UDPCONN=lib/net/udpconnection.cpp
if [ -n "$QEMU" ]; then
  awk -v head='int CUDPConnection::PacketReceived (CNetBuffer *pPacket,' '
    index($0, head) == 1 { h++; on = 1 }
    { print }
    on && $0 == "{" { print "\tif (!m_bOpen)\t\t// pix68k (#136): a closed connection must not consume packets"; print "\t{"; print "\t\treturn 0;"; print "\t}"; print ""; on = 0; m++ }
    END { if (h != 1 || m != 1) exit 1 }' "$ROOT/external/circle/$UDPCONN" > "$UDPCONN.tmp" ||
    { echo "CUDPConnection::PacketReceived() not found once in $UDPCONN" >&2; exit 1; }
else
  cp "$ROOT/external/circle/$UDPCONN" "$UDPCONN.tmp"
fi
if cmp -s "$UDPCONN.tmp" "$UDPCONN"; then rm "$UDPCONN.tmp"; else mv "$UDPCONN.tmp" "$UDPCONN"; fi

# カーネルの大きさの上限 (Circle の KERNEL_MAX_SIZE。MB の単位。既定は 2MB)。.bss を含めた終わり (_end) が
# 0x80000 + この値を超えると、Circle の sysinit() が何も表示せずに止まる。Pi 3 / Pi 4 / Pi 5 は 68030 / 68060 の命令表
# (512KB ずつ) などで 2MB を超えるので 8MB にする。Circle の configure --kernel-max-size で渡す (Config.mk の DEFINE に
# -DKERNEL_MAX_SIZE が入り、Circle のライブラリとアプリの C++ とアセンブラのすべてに渡る。KASAN を使うときの shadow の
# 位置 (KASAN_SHADOW_MAPPING_OFFSET) も configure がこの値から決める)。
# 2 にすると、configure は Circle の既定と同じなので -DKERNEL_MAX_SIZE を書かない。下の Config.mk の確かめでは、
# そのときは 1 つもないことを確かめる。ただし今のカーネルは 2MB を超えるので、最後の _end の検査で必ず失敗する
KERNEL_MAX_SIZE_MB=8
KERNEL_MAX_SIZE=$((KERNEL_MAX_SIZE_MB * 0x100000))
OPTS="-r $N -p $PREFIX --multicore --keymap US --kernel-max-size $KERNEL_MAX_SIZE_MB -f $QEMU"
SHIM=$ROOT/baremetal/shim
ADDONS="addon/SDCard addon/fatfs"
[ "$N" = 5 ] && ADDONS="$ADDONS addon/nvme"
# Linux 向けの GCC (CI の baremetal の job。PREFIX64=aarch64-linux-gnu-) かどうか。下で Config.mk に書くオプションが変わる
command -v "${PREFIX}gcc" >/dev/null 2>&1 ||
  { echo "コンパイラ ${PREFIX}gcc がない (PREFIX64 で接頭辞を変えられる。例: PREFIX64=aarch64-linux-gnu-)" >&2; exit 1; }
case $("${PREFIX}gcc" -dumpmachine) in
  *-linux-*) LINUX_GCC=1 ;;
  *) LINUX_GCC= ;;
esac
# 設定 (shim の場所を含む) が変わったときだけ作り直す
CONFIG="$OPTS shim=$SHIM depth=32 fastseek=1 nocalibrate=1 msdelayhook=1 qemudwc2nak=1 qemuudpclosed=1${LINUX_GCC:+ nooutlineatomics=1 nounwind=1 nopie=1}"
if [ ! -f .pix68k-config ] || [ "$(cat .pix68k-config)" != "$CONFIG" ]; then
  # 設定をやり直している間は .pix68k-config を置かない。途中で失敗したとき、次のビルドが前の設定のまま
  # configure を飛ばさないようにする (書くのは、この if の最後まで成功したとき)
  rm -f .pix68k-config
  ./configure $OPTS >/dev/null
  # configure が KERNEL_MAX_SIZE を 1 つだけ書いたことを確かめる (configure の書き方が変わったときに気づくため)。
  # 2MB のときは configure が書かないので、1 つもないことを確かめる
  if [ "$KERNEL_MAX_SIZE_MB" -eq 2 ]; then
    KMAX_DEF=
  else
    KMAX_DEF=-DKERNEL_MAX_SIZE=$(printf '0x%X' $KERNEL_MAX_SIZE)
  fi
  KMAX_FOUND=$(awk '{ for (i = 1; i <= NF; i++) if (index($i, "-DKERNEL_MAX_SIZE=") == 1) print $i }' Config.mk)
  [ "$KMAX_FOUND" = "$KMAX_DEF" ] ||
    { echo "Config.mk の -DKERNEL_MAX_SIZE が '$KMAX_DEF' のはずが '$KMAX_FOUND' だった" >&2; exit 1; }
  # Homebrew の aarch64-elf-gcc は C ライブラリのヘッダを持たないので、<string.h> などを Circle の関数で置き換える
  echo "EXTRAINCLUDE += -I $SHIM" >> Config.mk
  # 画面は 32 ビット (X68000 の画面を XRGB8888 のまま写す)。DEPTH は Circle のライブラリとアプリで揃える
  echo "DEFINE += -DDEPTH=32" >> Config.mk
  # 遅延ループの較正 (CTimer::TuneMsDelay()) をしない。起動のたびに約 1 秒分のループを回す (Pi 3 の 600MHz で
  # 0.5 秒ほど、QEMU で 0.2 秒) が、結果を使うのは nsDelay() だけで、Circle も pix68k も使っていない
  # (Circle の include/circle/sysconfig.h の CALIBRATE_DELAY の注釈を参照)
  echo "DEFINE += -DNO_CALIBRATE_DELAY" >> Config.mk
  # Linux 向けの GCC (aarch64-linux-gnu-) では、アトミックな操作 (__atomic_*) を、libgcc の関数の呼び出しにせず、
  # その場の命令にする。LSE のない CPU (Pi 3 の Cortex-A53、Pi 4 の Cortex-A72) では libgcc の関数 (__aarch64_cas4 など)
  # を呼ぶのが GCC の既定で、Linux 向けの libgcc のその関数は、LSE の有無を __getauxval() で調べるので、ベアメタルでは
  # リンクできない。aarch64-elf- の libgcc は __getauxval() を使わずリンクできるので、そちらのビルドは変えない。
  # アプリの部分には baremetal/Makefile が同じオプションを付ける (速さのため。どのコンパイラでも)。ここで足すのは、
  # Circle のライブラリにも渡すため (ライブラリに libgcc の関数の呼び出しが残ると、やはりリンクできない)
  if [ -n "$LINUX_GCC" ]; then echo "CFLAGS += -mno-outline-atomics" >> Config.mk; fi
  # Linux 向けの GCC は、例外を使わないコードにも、スタックを巻き戻すための表 (.eh_frame) を作る
  # (-fasynchronous-unwind-tables が既定)。ベアメタルでは誰も読まないのに、カーネルに入って約 300KB を占めていた (Pi 5)。
  # Ubuntu の GCC は、位置に依存しないコード (-fPIE) も既定で、大域変数の番地を表 (.got) から読む命令が出る。
  # どちらも aarch64-elf- の GCC にはないので、そちらに合わせて止める (#204)
  if [ -n "$LINUX_GCC" ]; then
    echo "CFLAGS += -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-pie" >> Config.mk
  fi
  # Circle の USE_NAK_USB_FIX は入れない (QEMU 用のビルドでも。上の dwhcidevice.cpp の書き換えを参照)
  ./makeall clean >/dev/null 2>&1 || true
  for d in $ADDONS; do make -C "$d" clean >/dev/null 2>&1 || true; done
  # 古い依存ファイル (.d) が残っていると、消えたヘッダを探して make が止まることがあるので消す
  find . -name '*.d' -delete
  echo "$CONFIG" > .pix68k-config
fi
./makeall --nosample -j8 >/dev/null
for d in $ADDONS; do make -C "$d" -j8 >/dev/null; done

# アプリは毎回写し直す (--delete で app/ の生成物も消えるので、毎回すべてビルドし直す。3 ファイルなので数秒)
rsync -a --delete "$ROOT/baremetal/" "$OUT/app/"
# 68030 / 68060 のコアは Pi 3 / Pi 4 / Pi 5 のすべてに入れる。PIX68K_CPU020=0 を付けて実行すると入れない (68000 だけ)
case "${PIX68K_CPU020:-1}" in
  1) CPU020=1 ;;
  0) CPU020= ;;
  *) echo "PIX68K_CPU020 は 0 か 1: ${PIX68K_CPU020}" >&2; exit 2 ;;
esac
# アプリの make がイメージを作ったあとで失敗したときも (set -e で抜ける)、検査していないイメージを残さない
trap 'rm -f "$OUT"/app/kernel*.img' EXIT
make -C "$OUT/app" -j8 CIRCLEHOME="$OUT/circle" PIX68K_SRC="$ROOT/src" PIX68K_QEMU=${QEMU:+1} PIX68K_CPU020=$CPU020
ls -l "$OUT"/app/kernel*.img
# _end が上限を超えていたら失敗にする (超えたカーネルは実機で何も表示せずに止まり、原因が分かりにくい)。
# 確かめられなかったとき (ELF がない、nm が失敗した、_end がない) も失敗にする。どちらでも、誤って SD カードに
# 写さないように、できたばかりのイメージを消す
fail() {
  echo "$1" >&2
  rm -f "$OUT"/app/kernel*.img
  exit 1
}
# Circle の Rules.mk がビルド先ごとに決める名前 (app/ は毎回写し直すので、ほかの名前の ELF は残らない)
case $N in
  3) KERNEL=kernel8 ;;
  4) KERNEL=kernel8-rpi4 ;;
  5) KERNEL=kernel_2712 ;;
esac
ELF=$OUT/app/$KERNEL.elf
[ -f "$ELF" ] || fail "$ELF がない"
set -- "$OUT"/app/kernel*.elf
[ $# -eq 1 ] || fail "$OUT/app に ELF が $# 個ある (1 つのはず): $*"
SYMS=$(${PREFIX}nm "$ELF") || fail "${PREFIX}nm $ELF が失敗した"
END=$(printf '%s\n' "$SYMS" | awk '$3 == "_end" { print $1 }')
case $END in
  ''|*[!0-9a-fA-F]*) fail "_end が $ELF にない (nm の答え: '$END')" ;;
esac
LIMIT=$((0x80000 + KERNEL_MAX_SIZE))
if [ $((0x$END)) -gt $LIMIT ]; then
  fail "$(printf '%s の _end (0x%x) が Circle の上限 (0x%x = 0x80000 + KERNEL_MAX_SIZE) を超えている。KERNEL_MAX_SIZE_MB を大きくすること' \
    "$ELF" $((0x$END)) $LIMIT)"
fi
printf '_end 0x%x (上限 0x%x、残り %d KB)\n' $((0x$END)) $LIMIT $(((LIMIT - 0x$END) / 1024))
# CPU のコアと画面の合成の関数に memcpy() の呼び出しが残っていたら失敗にする (baremetal/builtin_mem.h が読まれなく
# なったときなどに気づくため)。memcpy() / memset() (baremetal/mem_fast.cpp) が、ほかの関数、NEON、スタックを使って
# いないことと、Circle のものがリンクされていないことも調べる (tools/baremetal_hot_mem.sh の説明を参照)
HOT=$(sh "$ROOT/tools/baremetal_hot_mem.sh" "$OUT/app/$KERNEL.lst" "$OUT/app/$KERNEL.map") ||
  fail "memcpy() / memset() の検査 (tools/baremetal_hot_mem.sh) で見つかった:
$HOT"
trap - EXIT
