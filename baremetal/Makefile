# ベアメタル版 (Circle) のアプリケーション。tools/build_baremetal.sh から呼ぶ。
#   make CIRCLEHOME=<設定・ビルド済みの Circle> PIX68K_SRC=<リポジトリの src>

CIRCLEHOME ?= ../external/circle
PIX68K_SRC ?= ../src

# X68000 本体 (src/x68k)
X68K_OBJS = x68k_machine.o x68k_mfp.o x68k_rtc.o x68k_video.o x68k_devices.o x68k_render.o x68k_gvram.o \
	x68k_fdd.o x68k_fdc.o x68k_dmac.o x68k_keyboard.o x68k_textscreen.o x68k_sram_file.o \
	x68k_scc.o x68k_mouse.o x68k_ppi.o x68k_sasi.o x68k_spc.o x68k_cd_drive.o x68k_opm.o x68k_adpcm.o x68k_sound.o x68k_video_replay.o x68k_sound_replay.o x68k_state.o \
	x68k_ts6bga.o x68k_fpu_board.o x68k_mercury.o x68k_neptune.o x68k_ym3802.o

# FPU の計算の部分 (FpuCore) は、68000 の機種の数値演算プロセッサボード (x68k/fpu_board.h) も使うので、常に入れる
OBJS	= main.o mem_fast.o kernel.o emulator.o machine_run.o machine_menu.o boot_menu.o net_bridge.o host_net.o pi5_fan.o m68k.o m68k_fpu_core.o m68k_fpu_math.o $(X68K_OBJS) \
	platform_sf2_synth.o platform_mt32_synth.o munt_libc.o $(MUNT_OBJS) platform_sc55_synth.o $(NUKED_OBJS)

# MT-32 の音源 Munt (external/munt、LGPL 2.1 以降)。ソースは変えずに、C++ の標準ライブラリと libm のかわりに
# shim_munt と munt_libc.cpp を使う。Munt の中の警告は出さない (-w。Munt のソースは直さない)。
# 標本化周波数の変換 (srchelper) は入れない: ResamplerModel.cpp が dynamic_cast を使い、RTTI なしでは
# ビルドできない。ベアメタル版の出力は 48kHz で、Munt の AnalogOutputMode_ACCURATE がそのまま 48kHz を出すので
# 変換は使わない (platform/mt32_synth.cpp は、変換が要る周波数では開かない)
MUNT_DIR = $(PIX68K_SRC)/../external/munt/mt32emu/src
MUNT_NAMES = Analog BReverbModel Display File LA32FloatWaveGenerator LA32Ramp LA32WaveGenerator MidiStreamParser \
	Part Partial PartialManager Poly ROMInfo Synth Tables TVA TVF TVP SampleRateConverter sha1
MUNT_OBJS = $(patsubst %,munt_%.o,$(MUNT_NAMES))
MUNT_INCLUDE = -I $(MUNT_DIR) -I $(PIX68K_SRC)/../external/munt/config -I shim_munt
MUNT_DEFINE =

# SC-55mkII の音源 Nuked-SC55 (external/nuked-sc55、GPL 2 以降。SDL の部分を除いたもの)。SDL のヘッダのかわりに
# platform/sc55_shim を、<stdio.h> のかわりに shim_sc55 を使う (printf は munt_libc.cpp の何も出さないもの)。
# Nuked-SC55 の中の警告は出さない (-w)。重いので -O3 にする (Pi 5 の Linux で 1.6 倍が 1.9 倍になった)
NUKED_DIR = $(PIX68K_SRC)/../external/nuked-sc55/src
NUKED_NAMES = mcu mcu_interrupt mcu_opcodes mcu_timer pcm submcu
NUKED_OBJS = $(patsubst %,nuked_%.o,$(NUKED_NAMES))
NUKED_INCLUDE = -I $(NUKED_DIR) -I $(PIX68K_SRC)/platform/sc55_shim -I shim_sc55

# 68030 / 68060 のコアと FPU (X68030、060turbo)。PIX68K_CPU020=1 のときだけ入れる (tools/build_baremetal.sh は既定で入れる。PIX68K_CPU020=0 なら入れず、68000 だけになる)
ifeq ($(strip $(PIX68K_CPU020)),1)
OBJS	+= m68k_cpu020.o m68k_fpu.o
else
DEFINE	+= -DPIX68K_NO_CPU020
endif

LIBS	= $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
	  $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
	  $(CIRCLEHOME)/lib/usb/libusb.a \
	  $(CIRCLEHOME)/lib/input/libinput.a \
	  $(CIRCLEHOME)/lib/fs/libfs.a \
	  $(CIRCLEHOME)/lib/sound/libsound.a \
	  $(CIRCLEHOME)/lib/net/libnet.a \
	  $(CIRCLEHOME)/lib/sched/libsched.a \
	  $(CIRCLEHOME)/lib/libcircle.a

INCLUDE	+= -I $(PIX68K_SRC) -I $(CIRCLEHOME)/addon
DEFINE	+= -DPIX68K_CIRCLE
# QEMU 用のビルド (tools/build_baremetal.sh 3 --qemu) では音の出力の既定を off にする
ifeq ($(strip $(PIX68K_QEMU)),1)
DEFINE	+= -DPIX68K_QEMU
endif
# アプリの部分はすべて -O3 にする (ホスト版の Release と同じ。Circle のライブラリは Circle の既定の -O2 のまま)。
# Pi 5 の 060turbo 75MHz で OutRun (Cannonball) のデモを動かすと、-O2 より、フレームの時間に間に合わなかった数が 7.5% 減り、
# 画面の合成 (コア 2) と音の合成 (コア 3) の 1 フレームの最大の時間も減った
OPTIMIZE = -O3

include $(CIRCLEHOME)/Rules.mk

# アプリの部分はフレームポインタを持たない (#224。ホスト版の Release と同じ)。Circle の Rules.mk は、KASAN のとき
# だけ -fno-omit-frame-pointer を付け、ふだんは何も付けないので、aarch64 の GCC の既定で、関数を呼ぶ関数ごとに x29 を
# 設定している。Circle のライブラリは変えない (この Makefile がビルドするものだけ)。例外のときに Circle が出す
# スタックの表示 (lib/debug.cpp の DebugStackTrace()) は、スタックの中のコードのアドレスらしい値を並べるもので、
# x29 をたどらないので変わらない
CFLAGS += -fomit-frame-pointer

# Circle の Rules.mk は -ffreestanding を付けるので、memcpy / memset などが、大きさが定数の 4 バイトや 8 バイトの
# ものも Circle の関数の呼び出しになる。builtin_mem.h をすべてのソースに読ませて、大きさが定数の小さいものを
# その場に展開させる (builtin_mem.h の説明を参照)。読ませるのはこの Makefile がビルドするもの (pix68k、Munt、
# Nuked-SC55、TinySoundFont) だけで、Circle のライブラリと addon (FatFs など) のビルドには渡らない。
# 大きさが変数のものは関数の呼び出しのまま残るので、memcpy() と memset() は mem_fast.cpp のものを使う
# (OBJS にあるので、リンクのときに Circle の lib/util_fast.S のものより先に使われる)
# CFLAGS に足す (Circle の Rules.mk では CPPFLAGS が CFLAGS を含むので、C と C++ のどちらのソースにも渡る)
BUILTIN_MEM = -include builtin_mem.h
CFLAGS += $(BUILTIN_MEM)
# 組み込み関数の memcpy などは、引数がヌルでないものとして扱われる (長さが 0 でも)。写したあとでポインタがヌルか
# どうかを調べているコードがあると、GCC はその検査を消すので、消させない
CFLAGS += -fno-delete-null-pointer-checks
# builtin_mem.h を読ませないもの: memcpy() / memset() そのもの (mem_fast.cpp の先頭の説明を参照)
NOBUILTIN_CPPFLAGS = $(filter-out $(BUILTIN_MEM),$(CPPFLAGS))
$(OBJS): builtin_mem.h

# Pi 3 / Pi 4 (LSE のない Cortex-A53 / A72) 向けでは、GCC の既定で、アトミックな操作 (__atomic_exchange_n、
# __atomic_compare_exchange_n、__atomic_fetch_add など) が libgcc の関数 (__aarch64_cas4_acq_rel など。outline atomics) の
# 呼び出しになる。この関数は呼ばれるたびに LSE の有無の変数を読んでから LL/SC のループに入る。ベアメタルでは LSE の
# 有無がビルドのときに決まっているので、その場の LL/SC の命令にする。Pi 5 (Cortex-A76) 向けは、付けなくても LSE の
# 命令がその場に出ていて、libgcc の関数の呼び出しはもともとない
CFLAGS += -mno-outline-atomics

mem_fast.o: mem_fast.cpp
	@echo "  CPP   $@"
	@$(CPP) $(NOBUILTIN_CPPFLAGS) -fno-builtin -fno-tree-loop-distribute-patterns -fno-tree-vectorize -mstrict-align -c -o $@ $<

# Pi 5 は boot パーティションが NVMe にある (Rules.mk を読んだあとなら RASPPI が分かる)
ifeq ($(strip $(RASPPI)),5)
LIBS	:= $(CIRCLEHOME)/addon/nvme/libnvme.a $(LIBS) $(CIRCLEHOME)/lib/sched/libsched.a
endif

# CPU コアは別ディレクトリにあり、Circle の規則では依存ファイル (.d) が作られないので、ヘッダを直接並べる
M68K_DEPS = $(wildcard $(PIX68K_SRC)/m68k/*.h $(PIX68K_SRC)/m68k/*.inc)

m68k.o: $(PIX68K_SRC)/m68k/m68k.cpp $(M68K_DEPS)
	@echo "  CPP   $@"
	@$(CPP) $(CPPFLAGS) -c -o $@ $<

# Cpu020 は FPU を new するので、確保の失敗を nullptr で受け取れるようにする
m68k_%.o: $(PIX68K_SRC)/m68k/%.cpp $(M68K_DEPS)
	@echo "  CPP   $@"
	@$(CPP) $(CPPFLAGS) -fcheck-new -c -o $@ $<

# -fcheck-new: Circle の new は確保に失敗すると 0 を返すので、Machine::ok() で失敗を返せるようにする
X68K_DEPS = $(wildcard $(PIX68K_SRC)/x68k/*.h $(PIX68K_SRC)/m68k/*.h)

x68k_%.o: $(PIX68K_SRC)/x68k/%.cpp $(X68K_DEPS)
	@echo "  CPP   $@"
	@$(CPP) $(CPPFLAGS) -fcheck-new -c -o $@ $<

# MIDI の SoundFont の音源 (TinySoundFont を含む)。TinySoundFont は malloc で確保し、失敗すると 0 を返す
platform_sf2_synth.o: $(PIX68K_SRC)/platform/sf2_synth.cpp $(PIX68K_SRC)/platform/sf2_synth.h \
		$(PIX68K_SRC)/platform/midi_stream.h $(PIX68K_SRC)/x68k/midi_out.h $(PIX68K_SRC)/../external/tinysoundfont/tsf.h
	@echo "  CPP   $@"
	@$(CPP) $(CPPFLAGS) -c -o $@ $<

# Munt。音を作る間に確保しないよう、platform/mt32_synth.cpp が Synth::open() の前に設定する
vpath %.cpp $(MUNT_DIR) $(MUNT_DIR)/sha1
MUNT_DEPS = $(wildcard $(MUNT_DIR)/*.h $(MUNT_DIR)/sha1/*.h) \
	$(PIX68K_SRC)/../external/munt/config/config.h $(wildcard shim_munt/*)

munt_%.o: %.cpp $(MUNT_DEPS)
	@echo "  CPP   $@"
	@$(CPP) $(CPPFLAGS) $(MUNT_INCLUDE) $(MUNT_DEFINE) -w -c -o $@ $<

munt_libc.o: munt_libc.cpp
	@echo "  CPP   $@"
	@$(CPP) $(CPPFLAGS) -c -o $@ $<

# MT-32 の音源 (Munt を包むもの)
platform_mt32_synth.o: $(PIX68K_SRC)/platform/mt32_synth.cpp $(PIX68K_SRC)/platform/mt32_synth.h \
		$(PIX68K_SRC)/platform/midi_stream.h $(PIX68K_SRC)/x68k/midi_out.h $(MUNT_DEPS)
	@echo "  CPP   $@"
	@$(CPP) $(CPPFLAGS) $(MUNT_INCLUDE) $(MUNT_DEFINE) -c -o $@ $<

# Nuked-SC55
NUKED_DEPS = $(wildcard $(NUKED_DIR)/*.h $(PIX68K_SRC)/platform/sc55_shim/*.h shim_sc55/*)

nuked_%.o: $(NUKED_DIR)/%.cpp $(NUKED_DEPS)
	@echo "  CPP   $@"
	@$(CPP) $(CPPFLAGS) $(NUKED_INCLUDE) -O3 -w -c -o $@ $<

# SC-55mkII の音源 (Nuked-SC55 を包むもの)
platform_sc55_synth.o: $(PIX68K_SRC)/platform/sc55_synth.cpp $(PIX68K_SRC)/platform/sc55_synth.h \
		$(PIX68K_SRC)/platform/sf2_synth.h $(PIX68K_SRC)/x68k/midi_out.h $(NUKED_DEPS)
	@echo "  CPP   $@"
	@$(CPP) $(CPPFLAGS) $(NUKED_INCLUDE) -fcheck-new -c -o $@ $<

kernel.o: $(wildcard $(PIX68K_SRC)/bench/*.h $(PIX68K_SRC)/m68k/*.h)
kernel.o emulator.o machine_run.o machine_menu.o boot_menu.o host_net.o pi5_fan.o: $(X68K_DEPS) $(wildcard $(PIX68K_SRC)/platform/*.h) emulator.h kernel.h sound_out.h host_net.h pi5_fan.h
# Machine など (pi5_fan.o は CGPIOClock と CGPIOPin) を new するので、確保の失敗を nullptr で受け取れるようにする
kernel.o emulator.o machine_run.o machine_menu.o boot_menu.o pi5_fan.o: CPPFLAGS += -fcheck-new

-include $(DEPS)
