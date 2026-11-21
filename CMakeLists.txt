cmake_minimum_required(VERSION 3.20)
project(pix68k CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Release ではフレームポインタを持たない (#224)。aarch64 の GCC と Apple clang は、指定しなければ関数を呼ぶ関数ごとに
# x29 を設定する (Pi 5 の Linux の GCC 14.2 で、060turbo の OutRun の命令数が 1.1〜1.3% 減った)。
# perf record -g などでフレームポインタをたどって呼び出し元を見たいときは -DPIX68K_FRAME_POINTER=ON でビルドする
option(PIX68K_FRAME_POINTER "Release でもフレームポインタを持つ (perf record -g などのため)" OFF)
if(PIX68K_FRAME_POINTER)
  # 既定でフレームポインタを省くコンパイラ (x86_64 の clang の -O3 など) でも持たせる
  add_compile_options($<$<CONFIG:Release>:-fno-omit-frame-pointer>)
else()
  add_compile_options($<$<CONFIG:Release>:-fomit-frame-pointer>)
endif()

# ベアメタル (Circle) と同じ制約でビルドできることを確かめるため、コアは例外と RTTI なしでビルドする
# 68030 / 68060 のコア (cpu020.cpp) と FPU (fpu.cpp) も同じライブラリに入れる
add_library(m68k STATIC src/m68k/m68k.cpp src/m68k/cpu020.cpp src/m68k/fpu.cpp src/m68k/fpu_core.cpp
  src/m68k/fpu_math.cpp)
target_include_directories(m68k PUBLIC src)
target_compile_options(m68k PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)

add_executable(sst_runner tests/sst/sst_runner.cpp)
target_link_libraries(sst_runner m68k)

enable_testing()
set(SST_DIR "$ENV{HOME}/.cache/pix68k/sst/bin" CACHE PATH "SingleStepTests (変換済み) の場所")
add_test(NAME sst68000 COMMAND sh -c "${CMAKE_BINARY_DIR}/sst_runner -c -e ${CMAKE_SOURCE_DIR}/tests/sst/expected_failures.txt ${SST_DIR}/*.sst")

add_executable(cpu_test tests/unit/cpu_test.cpp)
target_link_libraries(cpu_test m68k)
add_test(NAME cpu_test COMMAND cpu_test)
add_executable(cpu020_test tests/unit/cpu020_test.cpp)
target_link_libraries(cpu020_test m68k)
add_test(NAME cpu020_test COMMAND cpu020_test)
# 68030 / 68060 の命令ごとのサイクル数と結果を、命令の処理を 68060 用と分ける前の値と比べる (#170)
add_executable(cpu020_cycles_test tests/unit/cpu020_cycles_test.cpp)
target_link_libraries(cpu020_cycles_test m68k)
add_test(NAME cpu020_cycles_test COMMAND cpu020_cycles_test)
add_executable(fpu882_test tests/unit/fpu882_test.cpp)
target_link_libraries(fpu882_test m68k)
add_test(NAME fpu882_test COMMAND fpu882_test)
add_executable(cpu060_test tests/unit/cpu060_test.cpp)
target_link_libraries(cpu060_test m68k)
add_test(NAME cpu060_test COMMAND cpu060_test)

# Human68k の実行ファイルを CPU のコアだけで動かす (XEiJ の instructiontest.x / fputest.x を動かす)
add_executable(run_x tests/m68k/run_x.cpp)
target_link_libraries(run_x m68k)
target_compile_options(run_x PRIVATE -Wall -Wextra)

# FPU のソフトウェア演算をホストの double と libm に照らして調べる
add_executable(fpu_math_test tests/m68k/fpu_math_test.cpp)
target_link_libraries(fpu_math_test m68k)
add_test(NAME fpu_math_test COMMAND fpu_math_test)
# XEiJ の instructiontest.x (実機の結果) / fputest.x (計算で求めた期待値。FMOVECR だけは実機の値) と比べる。
# 全部は時間がかかるので一部だけ。
# ファイルがなければ飛ばす。すべて動かすときは build/run_x --cpu 68030 instructiontest.x all など
set(XEIJ_DIR "$ENV{HOME}/x68k/xeij" CACHE PATH "XEiJ の場所 (misc/instructiontest.x、misc/fputest.x、data/IPLROM30.DAT)")
add_test(NAME m68030_instructiontest COMMAND sh ${CMAKE_SOURCE_DIR}/tests/m68k/xeij_test.sh $<TARGET_FILE:run_x> 68030
  ${XEIJ_DIR}/misc/instructiontest.x bfextu bfins cas2.l chk2.l divs.l divu.l mulu.l muls.l pack unpk rtd)
set_tests_properties(m68030_instructiontest PROPERTIES SKIP_RETURN_CODE 77)
# instructiontest.x の水準 hard の chk2 / cmp2。未定義の N と V の違いは easy では出ず、hard で出る
add_test(NAME m68030_instructiontest_hard COMMAND sh ${CMAKE_SOURCE_DIR}/tests/m68k/xeij_test.sh $<TARGET_FILE:run_x> 68030
  ${XEIJ_DIR}/misc/instructiontest.x hard chk2 cmp2)
set_tests_properties(m68030_instructiontest_hard PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME m68882_fputest COMMAND sh ${CMAKE_SOURCE_DIR}/tests/m68k/xeij_test.sh $<TARGET_FILE:run_x> 68030
  ${XEIJ_DIR}/misc/fputest.x fsqrt fsin fetox flogn fmovecr fbcc fmove.p)
set_tests_properties(m68882_fputest PROPERTIES SKIP_RETURN_CODE 77)
# 水準 hard の超越関数のうち、結果が ±1 や x に近く、丸めの方向を誤りやすいもの (fpu_math.cpp の wbelowOne()、tinyOdd())、
# 引数が π/2 の倍数に近く、引数を縮めた後の桁が足りなくなりやすいもの (fsin / fcos / ftan / fsincos、fpu_math.cpp の reduce())
add_test(NAME m68882_fputest_hard COMMAND sh ${CMAKE_SOURCE_DIR}/tests/m68k/xeij_test.sh $<TARGET_FILE:run_x> 68030
  ${XEIJ_DIR}/misc/fputest.x hard fetoxm1 ftanh fasin fatanh fsinh fsin fcos ftan fsincos)
set_tests_properties(m68882_fputest_hard PROPERTIES SKIP_RETURN_CODE 77)
# 68060。run_x には 060SP がないので、68060 にない命令はその場で実行する (Cpu020::ASSIST_ALWAYS)
add_test(NAME m68060_instructiontest COMMAND sh ${CMAKE_SOURCE_DIR}/tests/m68k/xeij_test.sh $<TARGET_FILE:run_x> 68060
  ${XEIJ_DIR}/misc/instructiontest.x bfextu bfins cas2.l chk2.l divs.l divu.l mulu.l muls.l movep.l rtd)
set_tests_properties(m68060_instructiontest PROPERTIES SKIP_RETURN_CODE 77)
# 水準 hard の chk / div。68060 の未定義のフラグ (CHK の Z / V / C、DIVx のゼロ除算とあふれの N / Z) は hard で出る
add_test(NAME m68060_instructiontest_hard COMMAND sh ${CMAKE_SOURCE_DIR}/tests/m68k/xeij_test.sh $<TARGET_FILE:run_x> 68060
  ${XEIJ_DIR}/misc/instructiontest.x hard chk.w chk.l divs.w divu.w divs.l divu.l divsl.l divul.l)
set_tests_properties(m68060_instructiontest_hard PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME m68060_fputest COMMAND sh ${CMAKE_SOURCE_DIR}/tests/m68k/xeij_test.sh $<TARGET_FILE:run_x> 68060
  ${XEIJ_DIR}/misc/fputest.x fsqrt fadd fmul fdiv fsadd fdmul fbcc fscc fmove.l)
set_tests_properties(m68060_fputest PROPERTIES SKIP_RETURN_CODE 77)
# Musashi との差分テスト (tests/m68k/musashi_diff.cpp)。Musashi はリポジトリに入れない。
# tools/fetch_musashi.sh で MUSASHI_DIR に Musashi を置き、m68kops.c を作っておくとビルドしてテストに足す。
# 既知の違い (tests/m68k/musashi_expected.txt) より多く違えば失敗する
set(MUSASHI_DIR "$ENV{HOME}/.cache/pix68k/musashi" CACHE PATH "Musashi の場所 (m68kops.c を作ったもの)。なければビルドしない")
if(MUSASHI_DIR AND EXISTS "${MUSASHI_DIR}/m68kops.c")
  enable_language(C)
  add_library(musashi STATIC ${MUSASHI_DIR}/m68kcpu.c ${MUSASHI_DIR}/m68kops.c ${MUSASHI_DIR}/m68kdasm.c
    ${MUSASHI_DIR}/softfloat/softfloat.c)
  target_include_directories(musashi PUBLIC ${MUSASHI_DIR})
  target_compile_definitions(musashi PUBLIC M68K_EMULATE_PMMU=0)
  # Musashi のビットフィールドの命令には符号付きの桁あふれ (未定義動作) があり、最適化の度合いで結果が変わる。
  # -fwrapv で 2 の補数の回り込みに決め、ビルドの種類 (Release / サニタイザ入りの Debug) によらず同じ結果にする
  target_compile_options(musashi PRIVATE -w -fwrapv)
  add_executable(musashi_diff tests/m68k/musashi_diff.cpp)
  target_link_libraries(musashi_diff m68k musashi)
  add_test(NAME musashi_diff COMMAND musashi_diff -n 200000 -v 2 -e ${CMAKE_SOURCE_DIR}/tests/m68k/musashi_expected.txt)
endif()

add_executable(bench tests/bench/bench.cpp)
target_link_libraries(bench m68k)
# 68030 / 68060 のコアだけの速さを測る (テストではない)
add_executable(cpu020_bench tests/bench/cpu020_bench.cpp)
target_link_libraries(cpu020_bench m68k)

# X68000 本体 (メモリマップと周辺機器)。コアと同じく例外と RTTI なしでビルドする
add_library(x68k STATIC src/x68k/machine.cpp src/x68k/mfp.cpp src/x68k/rtc.cpp src/x68k/video.cpp src/x68k/devices.cpp
  src/x68k/gvram.cpp src/x68k/render.cpp
  src/x68k/fdd.cpp src/x68k/fdc.cpp src/x68k/dmac.cpp src/x68k/keyboard.cpp src/x68k/textscreen.cpp src/x68k/sram_file.cpp
  src/x68k/scc.cpp src/x68k/mouse.cpp src/x68k/ppi.cpp src/x68k/sasi.cpp src/x68k/spc.cpp src/x68k/cd_drive.cpp
  src/x68k/opm.cpp src/x68k/adpcm.cpp src/x68k/sound.cpp src/x68k/video_replay.cpp src/x68k/sound_replay.cpp
  src/x68k/state.cpp src/x68k/fpu_board.cpp src/x68k/mercury.cpp src/x68k/neptune.cpp src/x68k/ym3802.cpp)
target_include_directories(x68k PUBLIC src)
# -fcheck-new: new が 0 を返す環境 (Circle) で、確保の失敗を Machine::ok() で返すため
target_compile_options(x68k PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti -fcheck-new)
target_link_libraries(x68k m68k)

# MT-32 の音源 Munt (external/munt/mt32emu、LGPL 2.1 以降)。ソースは変えずに静的ライブラリにする。
# config.h は external/munt/config に手で書いたもの。Munt の中の警告は出さない (Munt のソースは直さない)。
# ホスト版は出力の周波数が 48kHz とは限らないので、標本化周波数の変換 (srchelper) も入れる。
# srchelper の ResamplerModel.cpp は dynamic_cast を使うので、このライブラリは RTTI ありでビルドする
# (ベアメタル版は srchelper を入れない。baremetal/Makefile)
set(MUNT_SRC ${CMAKE_SOURCE_DIR}/external/munt/mt32emu/src)
add_library(mt32emu STATIC
  ${MUNT_SRC}/Analog.cpp ${MUNT_SRC}/BReverbModel.cpp ${MUNT_SRC}/Display.cpp ${MUNT_SRC}/File.cpp
  ${MUNT_SRC}/LA32FloatWaveGenerator.cpp ${MUNT_SRC}/LA32Ramp.cpp ${MUNT_SRC}/LA32WaveGenerator.cpp
  ${MUNT_SRC}/MidiStreamParser.cpp ${MUNT_SRC}/Part.cpp ${MUNT_SRC}/Partial.cpp ${MUNT_SRC}/PartialManager.cpp
  ${MUNT_SRC}/Poly.cpp ${MUNT_SRC}/ROMInfo.cpp ${MUNT_SRC}/Synth.cpp ${MUNT_SRC}/Tables.cpp ${MUNT_SRC}/TVA.cpp
  ${MUNT_SRC}/TVF.cpp ${MUNT_SRC}/TVP.cpp ${MUNT_SRC}/sha1/sha1.cpp ${MUNT_SRC}/SampleRateConverter.cpp
  ${MUNT_SRC}/srchelper/InternalResampler.cpp ${MUNT_SRC}/srchelper/srctools/src/FIRResampler.cpp
  ${MUNT_SRC}/srchelper/srctools/src/SincResampler.cpp ${MUNT_SRC}/srchelper/srctools/src/IIR2xResampler.cpp
  ${MUNT_SRC}/srchelper/srctools/src/LinearResampler.cpp ${MUNT_SRC}/srchelper/srctools/src/ResamplerModel.cpp)
target_include_directories(mt32emu PUBLIC ${MUNT_SRC} ${CMAKE_SOURCE_DIR}/external/munt/config)
target_compile_definitions(mt32emu PRIVATE MT32EMU_WITH_INTERNAL_RESAMPLER=1)
target_compile_options(mt32emu PRIVATE -w -fno-exceptions)
# Munt は負の数の左シフト (C++17 では未定義、C++20 からは定義された動作。GCC と Clang は算術シフトとして扱う) を
# 音の計算に使うので、UBSan のビルドでもこれだけは調べない (Munt のソースは直さない)
if(CMAKE_CXX_FLAGS MATCHES "undefined")
  target_compile_options(mt32emu PRIVATE -fno-sanitize=shift-base)
endif()

# SC-55mkII の音源 Nuked-SC55 (external/nuked-sc55、GPL 2 以降)。SDL の部分を除いたもの (external/nuked-sc55 の
# ファイルの先頭に変えたところを書いた)。SDL のヘッダのかわりに src/platform/sc55_shim を使う。
# Nuked-SC55 の中の警告は出さず、printf は nuked_quiet.h で黙らせる。重いので -O3 にする (Pi 5 で 1.6 倍が 1.9 倍になった)
set(NUKED_SRC ${CMAKE_SOURCE_DIR}/external/nuked-sc55/src)
add_library(nukedsc55 STATIC ${NUKED_SRC}/mcu.cpp ${NUKED_SRC}/mcu_interrupt.cpp ${NUKED_SRC}/mcu_opcodes.cpp
  ${NUKED_SRC}/mcu_timer.cpp ${NUKED_SRC}/pcm.cpp ${NUKED_SRC}/submcu.cpp)
target_include_directories(nukedsc55 PUBLIC ${NUKED_SRC} ${CMAKE_SOURCE_DIR}/src/platform/sc55_shim)
target_compile_options(nukedsc55 PRIVATE -w -O3 -fno-exceptions -fno-rtti
  -include ${CMAKE_SOURCE_DIR}/src/platform/sc55_shim/nuked_quiet.h)
# Nuked-SC55 は負の数の左シフトを計算に使う (Munt と同じく、UBSan のビルドでもこれだけは調べない)
if(CMAKE_CXX_FLAGS MATCHES "undefined")
  target_compile_options(nukedsc55 PRIVATE -fno-sanitize=shift-base)
endif()

# MIDI の音源: SoundFont (TinySoundFont、external/tinysoundfont)、MT-32 (Munt)、SC-55mkII (Nuked-SC55)。
# x68k と同じく例外と RTTI なしでビルドする
add_library(midisynth STATIC src/platform/sf2_synth.cpp src/platform/mt32_synth.cpp src/platform/sc55_synth.cpp)
target_include_directories(midisynth PUBLIC src)
target_compile_options(midisynth PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
target_link_libraries(midisynth mt32emu nukedsc55)

# MIDI ボード (YM3802) と音源。ステートの書き換えと乱数の差分試験は、未定義の動作でも止まるビルドでも確かめる:
#   cmake -S . -B build/san -DCMAKE_BUILD_TYPE=Debug \
#     "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all" \
#     "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all" \
#     -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined
#   cmake --build build/san --target midi_test && build/san/midi_test
add_executable(midi_test tests/unit/midi_test.cpp)
target_link_libraries(midi_test x68k midisynth)
target_compile_options(midi_test PRIVATE -Wall -Wextra)
add_test(NAME midi_test COMMAND midi_test)
# MT-32 の音源 (Munt)。ROM がなくても通る。本物の ROM で鳴ることも見るときは、環境変数で ROM を渡す:
#   PIX68K_MT32_CTRL=mt32_ctrl_1_07.rom PIX68K_MT32_PCM=mt32_pcm.rom
#   PIX68K_CM32L_CTRL=cm32l_ctrl_1_02.rom PIX68K_CM32L_PCM=cm32l_pcm.rom build/mt32_test
# midi_test と同じく、ASan / UBSan のビルド (上の手順) でも確かめる
add_executable(mt32_test tests/unit/mt32_test.cpp)
target_link_libraries(mt32_test x68k midisynth)
target_compile_options(mt32_test PRIVATE -Wall -Wextra)
add_test(NAME mt32_test COMMAND mt32_test)
# SC-55mkII の音源 (Nuked-SC55)。ROM がなくても通る。本物の ROM で鳴ることも見るときは、ROM のディレクトリを渡す:
#   PIX68K_SC55_ROM=sc55mk2 build/sc55_test
# midi_test と同じく、ASan / UBSan のビルド (上の手順) でも確かめる。非同期で pump() を別のスレッドから呼ぶ試験があるので、
# ROM を渡して -DCMAKE_CXX_FLAGS=-fsanitize=thread のビルドでも確かめる
add_executable(sc55_test tests/unit/sc55_test.cpp)
target_link_libraries(sc55_test x68k midisynth Threads::Threads)
target_compile_options(sc55_test PRIVATE -Wall -Wextra)
add_test(NAME sc55_test COMMAND sc55_test)
# UART から実機の MIDI 音源に送る部分 (platform/uart_midi.h)。midi_test と同じく、ASan / UBSan のビルドでも確かめる
add_executable(uart_midi_test tests/unit/uart_midi_test.cpp)
target_include_directories(uart_midi_test PRIVATE src)
target_link_libraries(uart_midi_test Threads::Threads)
target_compile_options(uart_midi_test PRIVATE -Wall -Wextra)
add_test(NAME uart_midi_test COMMAND uart_midi_test)
# ベアメタル版で Munt が使う数学の関数など (baremetal/munt_libc.cpp) をホストの libm と比べ、Munt の表が同じになるかを
# 見る。munt_libc.cpp は AArch64 の命令を使うので、AArch64 のホストだけ
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
  add_executable(munt_libc_test tests/unit/munt_libc_test.cpp tests/unit/munt_libc_host.cpp tests/unit/munt_libc_tables.cpp)
  target_link_libraries(munt_libc_test mt32emu)
  # 表は baremetal/shim_munt の <cmath> でビルドする
  set_source_files_properties(tests/unit/munt_libc_tables.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/baremetal/shim_munt" COMPILE_OPTIONS "-w")
  add_test(NAME munt_libc_test COMMAND munt_libc_test)
endif()
# ベアメタル版の memcpy() / memset() (baremetal/mem_fast.cpp) を、名前を変えてホストでビルドして試験する
add_executable(mem_fast_test tests/unit/mem_fast_test.cpp)
target_compile_options(mem_fast_test PRIVATE -Wall -Wextra)
add_test(NAME mem_fast_test COMMAND mem_fast_test)
# ベアメタル版のビルドの最後の検査 (tools/baremetal_hot_mem.sh) を、逆アセンブルの例を渡して試験する
add_test(NAME baremetal_hot_mem_test COMMAND sh ${CMAKE_SOURCE_DIR}/tests/tools/baremetal_hot_mem_test.sh
  ${CMAKE_SOURCE_DIR}/tools/baremetal_hot_mem.sh)

add_executable(x68k_test tests/unit/x68k_test.cpp)
target_link_libraries(x68k_test x68k)
add_test(NAME x68k_test COMMAND x68k_test)
add_executable(boards_test tests/unit/boards_test.cpp)
target_link_libraries(boards_test x68k)
add_test(NAME boards_test COMMAND boards_test)
# 68030 / 68060 で、MFP と OPM のタイマの割り込みが、要求の出た命令の切れ目で届くこと
add_executable(timer_irq_test tests/unit/timer_irq_test.cpp)
target_link_libraries(timer_irq_test x68k)
target_compile_options(timer_irq_test PRIVATE -Wall -Wextra)
add_test(NAME timer_irq_test COMMAND timer_irq_test)
add_executable(bus_wait_test tests/unit/bus_wait_test.cpp)
target_link_libraries(bus_wait_test x68k)
target_compile_options(bus_wait_test PRIVATE -Wall -Wextra)
add_test(NAME bus_wait_test COMMAND bus_wait_test)
add_executable(neptune_test tests/unit/neptune_test.cpp)
target_link_libraries(neptune_test x68k Threads::Threads)
target_compile_options(neptune_test PRIVATE -Wall -Wextra)
target_compile_definitions(neptune_test PRIVATE PIX68K_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME neptune_test COMMAND neptune_test)

add_executable(video_test tests/unit/video_test.cpp)
target_link_libraries(video_test x68k)
add_test(NAME video_test COMMAND video_test)
add_executable(video_regs_test tests/unit/video_regs_test.cpp)
target_link_libraries(video_regs_test x68k)
add_test(NAME video_regs_test COMMAND video_regs_test)
add_executable(crtc_op_test tests/unit/crtc_op_test.cpp)
target_link_libraries(crtc_op_test x68k)
add_test(NAME crtc_op_test COMMAND crtc_op_test)

add_executable(line_listener_test tests/unit/line_listener_test.cpp)
target_link_libraries(line_listener_test x68k)
target_compile_options(line_listener_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME line_listener_test COMMAND line_listener_test)

# 画面の合成 (Renderer) だけの速さを測る (テストではない)。場面は tests/bench/video_bench.cpp の先頭の表
set(VIDEO_BENCH_SOURCES tests/bench/video_bench.cpp)
add_executable(video_bench ${VIDEO_BENCH_SOURCES})
target_link_libraries(video_bench PRIVATE x68k)

# X68000 全体の速さと内訳を測る (テストではない)。Pi では tests/bench/machine_bench.cpp の先頭の手順でビルドする
find_package(Threads REQUIRED)
add_executable(machine_bench tests/bench/machine_bench.cpp)
target_link_libraries(machine_bench x68k Threads::Threads)
target_compile_options(machine_bench PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
# -s の値が正しくなければ、ROM を読む前に止まる (止まらずに動き続けないか)
foreach(v 0 -1 abc inf nan 1x 1e-9)
  add_test(NAME machine_bench_bad_seconds_${v} COMMAND machine_bench -s ${v} idle)
  set_tests_properties(machine_bench_bad_seconds_${v} PROPERTIES PASS_REGULAR_EXPRESSION "-s: want seconds" TIMEOUT 10)
endforeach()

# SoundFont の MIDI 音源の速さを測る (テストではない)。Pi では tests/bench/midi_bench.cpp の先頭の手順でビルドする
add_executable(midi_bench tests/bench/midi_bench.cpp)
target_link_libraries(midi_bench midisynth)
# MT-32 の音源 (Munt) の速さを測る (テストではない。ROM を渡す)。Pi では tests/bench/mt32_bench.cpp の先頭の手順でビルドする
add_executable(mt32_bench tests/bench/mt32_bench.cpp)
target_link_libraries(mt32_bench midisynth)
# SC-55mkII の音源 (Nuked-SC55) の速さを測る (テストではない。ROM のディレクトリを渡す)。Pi では
# tests/bench/sc55_bench.cpp の先頭の手順でビルドする
add_executable(sc55_bench tests/bench/sc55_bench.cpp)
target_link_libraries(sc55_bench midisynth)

# OPM の合成の速さを測る (テストではない)。Pi では tests/bench/opm_bench.cpp の先頭の手順でビルドする
add_executable(opm_bench tests/bench/opm_bench.cpp)
target_link_libraries(opm_bench x68k)

add_executable(fdc_test tests/unit/fdc_test.cpp)
target_link_libraries(fdc_test x68k)
add_test(NAME fdc_test COMMAND fdc_test)
add_executable(keyboard_test tests/unit/keyboard_test.cpp)
target_link_libraries(keyboard_test x68k)
add_test(NAME keyboard_test COMMAND keyboard_test)
add_executable(sram_file_test tests/unit/sram_file_test.cpp)
target_link_libraries(sram_file_test x68k)
add_test(NAME sram_file_test COMMAND sram_file_test)
add_executable(opm_test tests/unit/opm_test.cpp)
target_link_libraries(opm_test x68k)
add_test(NAME opm_test COMMAND opm_test)
add_executable(adpcm_test tests/unit/adpcm_test.cpp)
target_link_libraries(adpcm_test x68k)
add_test(NAME adpcm_test COMMAND adpcm_test)
foreach(t scc_test mouse_test ppi_test)
  add_executable(${t} tests/unit/${t}.cpp)
  target_link_libraries(${t} x68k)
  target_compile_options(${t} PRIVATE -Wall -Wextra)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
# ホスト版の RS-232C の TCP (SDL2 は使わないので、ホスト版を作らないときも試す)
add_executable(host_serial_test tests/unit/host_serial_test.cpp host/host_serial.cpp)
target_include_directories(host_serial_test PRIVATE host src)
target_link_libraries(host_serial_test x68k Threads::Threads)
target_compile_options(host_serial_test PRIVATE -Wall -Wextra)
add_test(NAME host_serial_test COMMAND host_serial_test)
add_executable(input_map_test tests/unit/input_map_test.cpp)
target_include_directories(input_map_test PRIVATE src)
target_compile_options(input_map_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME input_map_test COMMAND input_map_test)
add_executable(interval_handoff_test tests/unit/interval_handoff_test.cpp)
target_include_directories(interval_handoff_test PRIVATE src)
target_link_libraries(interval_handoff_test Threads::Threads)
target_compile_options(interval_handoff_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME interval_handoff_test COMMAND interval_handoff_test)
# 合成するコアが使った時間の統計の受け渡し (ベアメタル版の 10 秒ごとのログの core 2 / core 3 の行)
add_executable(core_work_test tests/unit/core_work_test.cpp)
target_include_directories(core_work_test PRIVATE src)
target_link_libraries(core_work_test Threads::Threads)
target_compile_options(core_work_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME core_work_test COMMAND core_work_test)
add_executable(frame_handoff_test tests/unit/frame_handoff_test.cpp)
target_include_directories(frame_handoff_test PRIVATE src)
target_link_libraries(frame_handoff_test Threads::Threads)
target_compile_options(frame_handoff_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME frame_handoff_test COMMAND frame_handoff_test)
add_executable(frame_scale_test tests/unit/frame_scale_test.cpp)
target_include_directories(frame_scale_test PRIVATE src)
target_compile_options(frame_scale_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME frame_scale_test COMMAND frame_scale_test)
add_executable(crt_effect_test tests/unit/crt_effect_test.cpp)
target_include_directories(crt_effect_test PRIVATE src)
target_compile_options(crt_effect_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME crt_effect_test COMMAND crt_effect_test)
# 同じ試験を、NEON の命令を使わない書き方で (aarch64 のホストでも、ほかの CPU と同じコードを確かめる)
add_executable(crt_effect_scalar_test tests/unit/crt_effect_test.cpp)
target_include_directories(crt_effect_scalar_test PRIVATE src)
target_compile_definitions(crt_effect_scalar_test PRIVATE PIX68K_CRT_NO_NEON)
target_compile_options(crt_effect_scalar_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME crt_effect_scalar_test COMMAND crt_effect_scalar_test)

add_executable(bezel_test tests/unit/bezel_test.cpp)
target_include_directories(bezel_test PRIVATE src)
target_compile_options(bezel_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME bezel_test COMMAND bezel_test)

add_executable(osd_test tests/unit/osd_test.cpp)
target_include_directories(osd_test PRIVATE src)
target_compile_options(osd_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME osd_test COMMAND osd_test)

# 画面の左上のメッセージ (platform/osd_message.h。POST /message)
add_executable(osd_message_test tests/unit/osd_message_test.cpp)
target_include_directories(osd_message_test PRIVATE src)
target_compile_options(osd_message_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME osd_message_test COMMAND osd_message_test)
add_executable(fan_curve_test tests/unit/fan_curve_test.cpp)
target_include_directories(fan_curve_test PRIVATE src)
target_compile_options(fan_curve_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME fan_curve_test COMMAND fan_curve_test)
add_executable(event_stream_test tests/unit/event_stream_test.cpp)
target_include_directories(event_stream_test PRIVATE src)
target_compile_options(event_stream_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME event_stream_test COMMAND event_stream_test)
add_executable(sample_clock_test tests/unit/sample_clock_test.cpp)
target_include_directories(sample_clock_test PRIVATE src)
target_compile_options(sample_clock_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME sample_clock_test COMMAND sample_clock_test)
add_executable(key_tracker_test tests/unit/key_tracker_test.cpp)
target_include_directories(key_tracker_test PRIVATE src)
target_compile_options(key_tracker_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME key_tracker_test COMMAND key_tracker_test)

add_executable(key_remap_test tests/unit/key_remap_test.cpp)
target_link_libraries(key_remap_test x68k)
target_include_directories(key_remap_test PRIVATE src)
target_compile_options(key_remap_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME key_remap_test COMMAND key_remap_test)

# 何台かの USB キーボードを合わせる処理と、Ctrl + Alt + Delete (ベアメタル版の再起動) の見分け方
add_executable(key_merge_test tests/unit/key_merge_test.cpp)
target_link_libraries(key_merge_test x68k)
target_include_directories(key_merge_test PRIVATE src)
target_compile_options(key_merge_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
# GCC (15 で確かめた) は、slot が 0 と決まった KeyMerger<2>::set() と KeyMerger<4>::set() の中身が同じになるので
# 1 つにまとめ (-fipa-icf)、KeyMerger<4> の関数が KeyMerger<2> の変数を触るとして -Warray-bounds の警告を出す。
# 触るのは slots_[0] だけで、範囲の外は読み書きしない (ASan / UBSan のビルドでも通る)。この試験だけ、まとめるのをやめる
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(key_merge_test PRIVATE -fno-ipa-icf)
endif()
add_test(NAME key_merge_test COMMAND key_merge_test)

# tools/fetch_musashi.sh: 同時に動いても壊れない、失敗しても作りかけを残さない (通信はしない。にせの curl を使う)
add_test(NAME fetch_musashi_test COMMAND sh ${CMAKE_SOURCE_DIR}/tests/tools/fetch_musashi_test.sh ${CMAKE_SOURCE_DIR}/tools/fetch_musashi.sh)

# ベアメタル版が、USB メモリのときの制限 (platform/boot_drive.h) を決まった入口から呼んでいること (ソースを見る)
add_test(NAME usb_guard_test COMMAND sh ${CMAKE_SOURCE_DIR}/tests/tools/usb_guard_test.sh ${CMAKE_SOURCE_DIR})

# pix68k.cfg の読み書きと項目の表 (platform/config_text.h、settings.h)、設定メニュー (platform/menu.h)、
# ベアメタル版のネットワークの口の部品 (platform/net_*.h、key_type.h)、読み書きするドライブの選び方 (platform/boot_drive.h)、
# 起動の組み合わせ (platform/boot_profile.h)
foreach(t config_test menu_test file_replace_test net_test boot_drive_test boot_profile_test)
  add_executable(${t} tests/unit/${t}.cpp)
  target_link_libraries(${t} x68k)
  target_compile_options(${t} PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# ディスクのイメージの書き戻し (x68k/dirty_map.h、platform/disk_sync.h)。2 つのスレッドで受け渡しも動かす
add_executable(disk_sync_test tests/unit/disk_sync_test.cpp)
target_link_libraries(disk_sync_test x68k Threads::Threads)
target_compile_options(disk_sync_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME disk_sync_test COMMAND disk_sync_test)

# SASI のハードディスク (x68k/sasi.h) と、内蔵 / 外付け SCSI の SPC (x68k/spc.h)。どちらも CPU を使わずに
# レジスタを読み書きして確かめる
foreach(t sasi_test spc_test)
  add_executable(${t} tests/unit/${t}.cpp)
  target_link_libraries(${t} x68k)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
# SCSI の CD-ROM 装置: CUE シート、MODE SENSE のページ、媒体の入れ替え、オーディオの再生と音の出力
add_executable(cdrom_test tests/unit/cdrom_test.cpp)
target_link_libraries(cdrom_test x68k)
target_compile_options(cdrom_test PRIVATE -Wall -Wextra)
add_test(NAME cdrom_test COMMAND cdrom_test)

# 実行状態の保存と復元 (x68k/state.h)。テスト用の IPL ROM で動かすものと、ROM と起動ディスクがあるときだけ動かす
# Human68k の起動の途中で保存するもの
add_executable(state_test tests/unit/state_test.cpp)
target_link_libraries(state_test x68k)
target_compile_options(state_test PRIVATE -Wall -Wextra)
add_test(NAME state_test COMMAND state_test)
# 壊れたステートを読ませる試験が、終わらない繰り返しに入っていないか (試験の中でも 1 回ごとに 60 秒で打ち切る)
set_tests_properties(state_test PROPERTIES TIMEOUT 600)

# 本物の IPL ROM で起動して画面を書き出す。ROM (~/.cache/pix68k/rom) がなければテストは飛ばす
add_executable(x68k_boot tests/x68k/boot_ipl.cpp)
target_link_libraries(x68k_boot x68k)
add_test(NAME x68k_boot_no_disk COMMAND x68k_boot -f 600 -o "" --check-no-disk)
set_tests_properties(x68k_boot_no_disk PROPERTIES SKIP_RETURN_CODE 77)
# Human68k を起動し、キーボードから DIR を打って一覧が出るかを見る。ROM か起動ディスクがなければ飛ばす
set(HUMAN68K_DISK "$ENV{HOME}/.cache/pix68k/disk/HUMAN302.XDF" CACHE FILEPATH "Human68k 3.02 の起動ディスク (XDF)")
add_test(NAME x68k_boot_type_dir COMMAND x68k_boot -0 ${HUMAN68K_DISK} -f 900 -o "" --type "dir\\n"
  --expect "A>dir" --expect "COMMAND            X")
set_tests_properties(x68k_boot_type_dir PROPERTIES SKIP_RETURN_CODE 77)
# 10MHz (初代〜SUPER) の設定でも Human68k が起動して DIR が打てること
add_test(NAME x68k_boot_10mhz COMMAND x68k_boot --clock 10 -0 ${HUMAN68K_DISK} -f 900 -o "" --type "dir\\n"
  --expect "A>dir" --expect "COMMAND            X")
set_tests_properties(x68k_boot_10mhz PROPERTIES SKIP_RETURN_CODE 77)
# X68030 (68030 25MHz、68882、IPL ROM 1.3) で Human68k が起動して DIR が打てること。IPLROM30.DAT がなければ飛ばす
set(X68030_IPLROM "${XEIJ_DIR}/data/IPLROM30.DAT" CACHE FILEPATH "X68030 の IPL ROM (IPLROM30.DAT)")
add_test(NAME x68k_boot_x68030 COMMAND x68k_boot --cpu 68030 --ipl ${X68030_IPLROM} -0 ${HUMAN68K_DISK} -f 900 -o ""
  --type "dir\\n" --expect "A>dir" --expect "COMMAND            X")
set_tests_properties(x68k_boot_x68030 PROPERTIES SKIP_RETURN_CODE 77)
# 060turbo (68060)。IPL ROM は ~/.cache/pix68k/rom/IPLROM060.DAT (68060 で動く IPL ROM) と ROM30_060.DAT が両方あれば
# IPLROM060.DAT を、なければ X68030 の IPL ROM を使う (テストを動かすときに tests/x68k/boot_060turbo.sh が決める)
set(TURBO060_SYS "$ENV{HOME}/.cache/pix68k/060turbo/060TURBO.SYS" CACHE FILEPATH "060turbo.sys (060turbo の配布物)")
add_test(NAME x68k_boot_060turbo_plain COMMAND sh ${CMAKE_SOURCE_DIR}/tests/x68k/boot_060turbo.sh $<TARGET_FILE:x68k_boot>
  ${X68030_IPLROM} ${HUMAN68K_DISK} -)
set_tests_properties(x68k_boot_060turbo_plain PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME x68k_boot_060turbo_sys COMMAND sh ${CMAKE_SOURCE_DIR}/tests/x68k/boot_060turbo.sh $<TARGET_FILE:x68k_boot>
  ${X68030_IPLROM} ${HUMAN68K_DISK} ${TURBO060_SYS})
set_tests_properties(x68k_boot_060turbo_sys PROPERTIES SKIP_RETURN_CODE 77)
# 060turbo.sys の拡張モード (-xm)。COMMAND.X の読み込み先がローカルメモリになる。フロッピーディスクからと、
# 内蔵 SCSI のハードディスク (tests/x68k/make_scsi_hds.py で作る) からの両方で起動する
add_test(NAME x68k_boot_060turbo_xm COMMAND sh ${CMAKE_SOURCE_DIR}/tests/x68k/boot_060turbo.sh $<TARGET_FILE:x68k_boot>
  ${X68030_IPLROM} ${HUMAN68K_DISK} ${TURBO060_SYS} "-xm")
add_test(NAME x68k_boot_060turbo_xm_scsi COMMAND sh ${CMAKE_SOURCE_DIR}/tests/x68k/boot_060turbo_scsi.sh
  $<TARGET_FILE:x68k_boot> ${X68030_IPLROM} ${HUMAN68K_DISK} ${TURBO060_SYS} ${CMAKE_SOURCE_DIR}/tools)
set_tests_properties(x68k_boot_060turbo_xm_scsi PROPERTIES SKIP_RETURN_CODE 77)
# 同じことを、68060 で動く IPL ROM 1.6 (IPLROM060.DAT) と、組で使う SCSI の ROM (ROM30_060.DAT) で。その SCSI IOCS は
# DMA で転送するので、DMAC のチャネル 1 をローカルメモリへ届ける互換の処理がないと止まる (#167)。ROM がなければ飛ばす
set(IPL16_ROM_DIR "$ENV{HOME}/.cache/pix68k/rom" CACHE PATH "IPLROM060.DAT と ROM30_060.DAT のディレクトリ")
add_test(NAME x68k_boot_060turbo_xm_scsi_iplrom16 COMMAND sh ${CMAKE_SOURCE_DIR}/tests/x68k/boot_060turbo_scsi.sh
  $<TARGET_FILE:x68k_boot> ${IPL16_ROM_DIR}/IPLROM060.DAT ${HUMAN68K_DISK} ${TURBO060_SYS} ${CMAKE_SOURCE_DIR}/tools
  ${IPL16_ROM_DIR}/ROM30_060.DAT)
set_tests_properties(x68k_boot_060turbo_xm_scsi_iplrom16 PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(x68k_boot_060turbo_xm PROPERTIES SKIP_RETURN_CODE 77)
# -xm -dv: デバイスドライバの転送を 060turbo.sys の転送バッファ経由にする (ローカルメモリのバッファへの転送を、
# DMA の届く転送バッファで中継する経路を動かす)
add_test(NAME x68k_boot_060turbo_xm_dv COMMAND sh ${CMAKE_SOURCE_DIR}/tests/x68k/boot_060turbo.sh $<TARGET_FILE:x68k_boot>
  ${X68030_IPLROM} ${HUMAN68K_DISK} ${TURBO060_SYS} "-xm -dv")
set_tests_properties(x68k_boot_060turbo_xm_dv PROPERTIES SKIP_RETURN_CODE 77)
# 060turbo を 75MHz (実機の 060turbo をオーバークロックした構成) にして、060turbo.sys なしとありで起動する
add_test(NAME x68k_boot_060turbo_75mhz_plain COMMAND sh ${CMAKE_SOURCE_DIR}/tests/x68k/boot_060turbo.sh $<TARGET_FILE:x68k_boot>
  ${X68030_IPLROM} ${HUMAN68K_DISK} - "" "--clock 75")
set_tests_properties(x68k_boot_060turbo_75mhz_plain PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME x68k_boot_060turbo_75mhz_sys COMMAND sh ${CMAKE_SOURCE_DIR}/tests/x68k/boot_060turbo.sh $<TARGET_FILE:x68k_boot>
  ${X68030_IPLROM} ${HUMAN68K_DISK} ${TURBO060_SYS} "" "--clock 75")
set_tests_properties(x68k_boot_060turbo_75mhz_sys PROPERTIES SKIP_RETURN_CODE 77)
# 印字できる ASCII をすべて打ち、プロンプトの行にそのまま出るかを見る (SHIFT の要る文字の対応の確認)
set(TYPE_LINE1 [=[ !"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ]=])
set(TYPE_LINE2 [=[[\]^_`abcdefghijklmnopqrstuvwxyz{|}~]=])
# 2 行は別々に起動して打つ (続けて打つと、1 行目のコマンドが終わってプロンプトに戻る瞬間に届いた SHIFT が
# Human68k に無視されることがある。キー入力の先打ちのタイミングによる)
foreach(n 1 2)
  add_test(NAME x68k_boot_type_ascii${n} COMMAND x68k_boot -0 ${HUMAN68K_DISK} -f 900 -o "" --after 200
    --type "${TYPE_LINE${n}}" --expect "A>${TYPE_LINE${n}}")
  set_tests_properties(x68k_boot_type_ascii${n} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
# SASI のディスクからの起動: Human68k のシステムディスクから make_sasi_hdf.py で HDF を作り、
# フロッピーディスクなしで起動して「A>」が出るのを待つ (システムディスクか ROM がなければ飛ばす)
add_test(NAME x68k_boot_sasi
  COMMAND sh -c
  "test -f '${HUMAN68K_DISK}' || exit 77; python3 -I '${CMAKE_SOURCE_DIR}/tools/make_sasi_hdf.py' '${HUMAN68K_DISK}' '${CMAKE_BINARY_DIR}/human302.hdf' || exit 1; '${CMAKE_BINARY_DIR}/x68k_boot' -f 900 -o '' -h '${CMAKE_BINARY_DIR}/human302.hdf' --expect 'A>'")
set_property(TEST x68k_boot_sasi PROPERTY SKIP_RETURN_CODE 77) # ディスクがないときは 77
add_test(NAME state_test_human68k COMMAND state_test -0 ${HUMAN68K_DISK})
set_tests_properties(state_test_human68k PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME state_test_human68k_x68030 COMMAND state_test -0 ${HUMAN68K_DISK} --cpu 68030 --ipl ${X68030_IPLROM})
set_tests_properties(state_test_human68k_x68030 PROPERTIES SKIP_RETURN_CODE 77)
# 060turbo で、060turbo.sys を組み込んだ Human68k (ディスクは boot_060turbo.sh と同じ作り方)
add_test(NAME state_test_human68k_060turbo COMMAND sh ${CMAKE_SOURCE_DIR}/tests/x68k/state_060turbo.sh $<TARGET_FILE:state_test>
  ${X68030_IPLROM} ${HUMAN68K_DISK} ${TURBO060_SYS})
set_tests_properties(state_test_human68k_060turbo PROPERTIES SKIP_RETURN_CODE 77)
# 数値演算プロセッサボードを、システムディスクの FLOAT3.X から使う (tests/x68k/boot_float3.sh)
add_test(NAME x68k_boot_float3 COMMAND sh ${CMAKE_SOURCE_DIR}/tests/x68k/boot_float3.sh $<TARGET_FILE:x68k_boot>
  ${HUMAN68K_DISK} ${CMAKE_BINARY_DIR}/human302_float3.xdf)
set_tests_properties(x68k_boot_float3 PROPERTIES SKIP_RETURN_CODE 77)

# X-BASIC で PRINT CHR$(7) を実行し、ベル (BEEP.SYS の ADPCM を DMAC のチャネル 3 で再生する) が鳴るかを見る
add_test(NAME x68k_boot_beep COMMAND x68k_boot -0 ${HUMAN68K_DISK} -f 900 -o "" --after 120
  --type "basic2\\basic\\n\\pprint chr$(7)\\n" --expect "print chr$(7)" --expect-sound 4000)
set_tests_properties(x68k_boot_beep PROPERTIES SKIP_RETURN_CODE 77)
# --time で時刻を固定すれば、2 回動かして書いた WAV (ベルの音) が同じになる
add_test(NAME x68k_boot_fixed_time COMMAND sh -c
  "for n in 1 2; do $<TARGET_FILE:x68k_boot> --time 1767225600 -0 '${HUMAN68K_DISK}' -f 600 -o '' --after 120 \
     --type 'basic2\\basic\\n\\pprint chr$(7)\\n' --expect 'print chr$(7)' --expect-sound 4000 \
     --wav fixed_time_$n.wav || exit $?; done; \
   cmp fixed_time_1.wav fixed_time_2.wav")
set_tests_properties(x68k_boot_fixed_time PROPERTIES SKIP_RETURN_CODE 77)
foreach(v abc -1 1x)
  add_test(NAME x68k_boot_bad_time_${v} COMMAND x68k_boot --time ${v})
  set_tests_properties(x68k_boot_bad_time_${v} PROPERTIES PASS_REGULAR_EXPRESSION "--time: want seconds" TIMEOUT 10)
endforeach()
# --time の値の検査 (先頭の空白や +、桁あふれを断り、0 と int64_t の最大は受け付ける。pix68k と同じ試験)
add_test(NAME x68k_boot_time_args COMMAND sh ${CMAKE_SOURCE_DIR}/tests/host/time_args_test.sh $<TARGET_FILE:x68k_boot> 1 "cannot read .*IPLROM.DAT" -f 1)
# tools/qemu_human68k.sh が見る video_scale=、video_filter= と前の書き方 (scale=、crt=、scanline=) のログの期待 (tools/qemu_crt_expect.sh) を確かめる
add_test(NAME qemu_crt_expect COMMAND sh ${CMAKE_SOURCE_DIR}/tests/host/qemu_crt_expect_test.sh)

# Human68k を起動し、RAM に置いたプログラムから IOCS のマウスの呼び出しを使い、ホストのマウスの動きと
# ボタンが届くかを見る。ROM か起動ディスクがなければ飛ばす
add_executable(mouse_iocs tests/x68k/mouse_iocs.cpp)
target_link_libraries(mouse_iocs x68k)
add_test(NAME x68k_mouse_iocs COMMAND mouse_iocs -0 ${HUMAN68K_DISK})
set_tests_properties(x68k_mouse_iocs PROPERTIES SKIP_RETURN_CODE 77)

# 外付け SCSI ボード (CZ-6BS1 相当) の ROM の SCSI IOCS から、CD-ROM 装置 (テスト用に作ったイメージ) が読めるか。
# SCSI の ROM はリポジトリに入れない。SCSI_EX_ROM に置いたファイル ($EA0000 / $EA0020 からの ROM) を使い、
# なければ飛ばす。Human68k の起動ディスクと IPL ROM がなくても飛ばす
set(SCSI_EX_ROM "$ENV{HOME}/.cache/pix68k/rom/SCSIEXROM.DAT" CACHE FILEPATH "外付け SCSI ボードの ROM")
add_executable(scsi_iocs tests/x68k/scsi_iocs.cpp)
target_link_libraries(scsi_iocs x68k)
add_test(NAME x68k_scsi_ex_cdrom COMMAND scsi_iocs --scsi-ex-rom ${SCSI_EX_ROM} -0 ${HUMAN68K_DISK})
set_tests_properties(x68k_scsi_ex_cdrom PROPERTIES SKIP_RETURN_CODE 77)
# 060turbo (68060 + ローカルメモリ) で、X68030 の IPL ROM の SCSI IOCS がローカルメモリのバッファを読み書きできるか
# (DMAC はローカルメモリに届かないので、ソフト転送にする。x68k/machine.cpp の patchIplFor060)。ROM がなければ飛ばす
add_executable(scsi_local_mem tests/x68k/scsi_local_mem.cpp)
target_link_libraries(scsi_local_mem x68k)
add_test(NAME x68k_scsi_local_mem COMMAND scsi_local_mem --ipl ${X68030_IPLROM})
set_tests_properties(x68k_scsi_local_mem PROPERTIES SKIP_RETURN_CODE 77)
# 同じことを、スーパーバイザスタックもローカルメモリに置いて行う
add_test(NAME x68k_scsi_local_mem_stack COMMAND scsi_local_mem --ipl ${X68030_IPLROM} --local-stack)
set_tests_properties(x68k_scsi_local_mem_stack PROPERTIES SKIP_RETURN_CODE 77)
# 同じことを、DMA の転送をまとめずに行う (既定ではまとめた転送でローカルメモリへ読む)
add_test(NAME x68k_scsi_local_mem_bytes COMMAND scsi_local_mem --ipl ${X68030_IPLROM} --no-dma-block)
set_tests_properties(x68k_scsi_local_mem_bytes PROPERTIES SKIP_RETURN_CODE 77)
# 060turbo で X68030 の IPL ROM を使うときの、IPL ROM が CACR に書く値の読み替え (起動時、IOCS _SYS_STAT の設定と取得、
# IOCS _MS_ONTM の待ちの間)
add_executable(iocs_wait060 tests/x68k/iocs_wait060.cpp)
target_link_libraries(iocs_wait060 x68k)
# 待ちの長さ: 1000 単位が 950ms から 1050ms (決まりの長さは 1 単位 1ms。x68k/machine.cpp の patchIplFor060 が、
# 待ちのループの回数を 68060 のサイクル数に合わせて書き換える)
add_test(NAME x68k_iocs_wait060 COMMAND iocs_wait060 --ipl ${X68030_IPLROM} --min 950 --max 1050)
set_tests_properties(x68k_iocs_wait060 PROPERTIES SKIP_RETURN_CODE 77)
# 75MHz でも同じ長さになる ($0CB8 をクロックに比例させ、待ちのループの回数はそこから決まる)
add_test(NAME x68k_iocs_wait060_75mhz COMMAND iocs_wait060 --ipl ${X68030_IPLROM} --min 950 --max 1050 --clock 75)
set_tests_properties(x68k_iocs_wait060_75mhz PROPERTIES SKIP_RETURN_CODE 77)
# 68060 で動く IPL ROM 1.6 は書き換えないので、1 単位は 1ms にならない (待ちのループの間はデータキャッシュが無効で、
# 1 周に本体の RAM を 9 回読む。50MHz で 1000 単位 約 2171ms。docs/060turbo.md の「68060 のサイクル数」と #123)。
# 本体のメモリの読み出しの時間 (Cpu020 の MISS060_MAIN) や、ループの間の CACR ($20808000。EDC が 0 でデータキャッシュは無効) が変わったら
# 分かるように、長さの範囲と CACR を確かめる。ROM がなければ飛ばす
set(IPLROM060 "$ENV{HOME}/.cache/pix68k/rom/IPLROM060.DAT" CACHE FILEPATH "68060 で動く IPL ROM (IPL ROM 1.6)")
set(ROM30_060 "$ENV{HOME}/.cache/pix68k/rom/ROM30_060.DAT" CACHE FILEPATH "IPL ROM 1.6 と組にする $FC0000 からの ROM")
add_test(NAME x68k_iocs_wait060_rom16 COMMAND iocs_wait060 --ipl ${IPLROM060} --rom30 ${ROM30_060} --any-rom
  --loop-cacr 20808000 --min 2050 --max 2300)
set_tests_properties(x68k_iocs_wait060_rom16 PROPERTIES SKIP_RETURN_CODE 77)
# 外付け SCSI のハードディスク (SCSI_EX_HDS。Human68k を入れた HDS) から、SASI の機種 (68000) が ROM 起動するか。
# SRAM の起動デバイスを ROM 起動 ($EA0020、ID 0) にして起動し、プロンプトが出るかを見る。イメージか ROM がなければ飛ばす
set(SCSI_EX_HDS "$ENV{HOME}/.cache/pix68k/disk/HUMAN302.HDS" CACHE FILEPATH "Human68k を入れた SCSI のハードディスクのイメージ")
add_test(NAME x68k_boot_scsi_ex COMMAND x68k_boot --scsi-ex-rom ${SCSI_EX_ROM} --se ${SCSI_EX_HDS} --boot-rom EA0020
  -f 1500 -o "" --expect "A>")
set_tests_properties(x68k_boot_scsi_ex PROPERTIES SKIP_RETURN_CODE 77)
# X68030 の内蔵 SCSI の ID 0 の HDS (SCSI30_HDS。Human68k を入れたもの) から、ベアメタル版の大きなイメージと同じく
# チャンクのキャッシュを通して (--stream。チャンクの読み込みは最大 1 フレーム待つ) 起動し、プロンプトが出るか。
# IPL ROM (IPLROM30.DAT) がなければ飛ばす。イメージがなければテストを作らない
set(SCSI30_HDS "$ENV{HOME}/.cache/pix68k/disk/HUMAN30.HDS" CACHE FILEPATH "X68030 で起動する Human68k を入れた SCSI のハードディスクのイメージ")
if(EXISTS "${SCSI30_HDS}")
  add_test(NAME x68k_boot_scsi30_stream COMMAND x68k_boot --cpu 68030 -s ${SCSI30_HDS} --stream -f 900 -o "" --expect "A>")
  set_tests_properties(x68k_boot_scsi30_stream PROPERTIES SKIP_RETURN_CODE 77)
endif()
# X68030 の内蔵 SCSI の ID 6 に CD-ROM (中身が 0 の 64 セクタのイメージ) をつないでも、IPL ROM の起動の処理が進んで
# フロッピーディスクの Human68k が起動するか。IPLROM30.DAT か起動ディスクがなければ飛ばす
add_test(NAME x68k_boot_x68030_cdrom COMMAND sh -c
  "head -c 131072 /dev/zero > x68030_cdrom.iso && '$<TARGET_FILE:x68k_boot>' --cpu 68030 --ipl '${X68030_IPLROM}' --cd x68030_cdrom.iso -0 '${HUMAN68K_DISK}' -f 900 -o '' --expect 'A>'")
set_tests_properties(x68k_boot_x68030_cdrom PROPERTIES SKIP_RETURN_CODE 77)

# 画面の合成を別のスレッドで行っても (x68k/video_replay.h)、フレームごとの画面が 1 つのスレッドのときと同じになるか。
# Human68k の起動と machine_bench の gfx の場面で調べる。ROM か起動ディスクがなければ飛ばす。
# -DCMAKE_CXX_FLAGS=-fsanitize=thread でビルドすると、スレッドの間のデータの競合も調べられる
add_executable(render_thread_test tests/x68k/render_thread_test.cpp)
target_link_libraries(render_thread_test x68k Threads::Threads)
target_compile_options(render_thread_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME render_thread_test COMMAND render_thread_test -0 ${HUMAN68K_DISK})
set_tests_properties(render_thread_test PROPERTIES SKIP_RETURN_CODE 77)

# 音の合成を別のスレッドで行っても (x68k/sound_replay.h)、作る音が 1 つのスレッドのときと同じになるか。
# Human68k の起動、machine_bench の gfx の場面 (OPM 8 チャネル)、ADPCM の再生で調べる。ROM か起動ディスクがなければ飛ばす
add_executable(sound_thread_test tests/x68k/sound_thread_test.cpp)
target_link_libraries(sound_thread_test x68k Threads::Threads)
target_compile_options(sound_thread_test PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
add_test(NAME sound_thread_test COMMAND sound_thread_test -0 ${HUMAN68K_DISK})
set_tests_properties(sound_thread_test PROPERTIES SKIP_RETURN_CODE 77)

# ホスト版 (SDL2)。SDL2 がなければ作らない
option(PIX68K_HOST "SDL2 のホスト版をビルドする" ON)
if(PIX68K_HOST)
  option(PIX68K_REQUIRE_SDL2 "SDL2 がなければエラーにする (CI 用)" OFF)
  if(PIX68K_REQUIRE_SDL2)
    find_package(SDL2 CONFIG REQUIRED)
  else()
    find_package(SDL2 CONFIG)
  endif()
  if(SDL2_FOUND)
    add_executable(pix68k host/main.cpp host/host_ether.cpp host/host_serial.cpp)
    target_include_directories(pix68k PRIVATE src)
    target_compile_options(pix68k PRIVATE -Wall -Wextra)
    if(WIN32 AND TARGET SDL2::SDL2main)
      target_link_libraries(pix68k SDL2::SDL2main)
      target_link_options(pix68k PRIVATE -mconsole)
    endif()
    target_link_libraries(pix68k x68k midisynth SDL2::SDL2 Threads::Threads)
    if(WIN32)
      target_link_options(pix68k PRIVATE -static-libgcc -static-libstdc++ -static)
    endif()
    # Neptune-X の neptune_link=pcap:IF に使う。なければ pcap の口なしで作る
    find_path(PCAP_INCLUDE_DIR pcap.h)
    find_library(PCAP_LIBRARY pcap)
    if(PCAP_INCLUDE_DIR AND PCAP_LIBRARY)
      target_compile_definitions(pix68k PRIVATE PIX68K_HAVE_PCAP)
      target_include_directories(pix68k PRIVATE ${PCAP_INCLUDE_DIR})
      target_link_libraries(pix68k ${PCAP_LIBRARY})
    endif()
    # ROM があれば X68000 を、なければテストパターンを 3 フレーム動かす。SRAM のファイルは読み書きしない
    add_test(NAME host_smoke COMMAND pix68k --no-config --frames 3 --no-sram)
    add_test(NAME host_smoke_pattern COMMAND pix68k --no-config --test-pattern --frames 3)
    # --config: ファイル名のキーの空の値 (sc6=、cd=) は指定しなかったことになり、ID の重なりのエラーにならない
    add_test(NAME host_config_empty_values COMMAND sh -c
      "printf 'sc6=\\ncd=\\nscsi_ex_rom=\\n' > host_config_empty.cfg && '$<TARGET_FILE:pix68k>' --config host_config_empty.cfg --frames 3 --no-sram --no-audio")
    # Neptune-X: 試験用の口 (echo) で起動する。知らない口と使えない MAC アドレスはエラーで終わる
    add_test(NAME host_neptune COMMAND pix68k --no-config --frames 3 --no-sram --no-audio --neptune)
    add_test(NAME host_neptune_bad_link COMMAND pix68k --no-config --frames 3 --no-sram --no-audio --neptune-link bogus)
    add_test(NAME host_neptune_bad_mac COMMAND pix68k --no-config --frames 3 --no-sram --no-audio --neptune --neptune-mac 01:00:00:00:00:00)
    # 誤りのメッセージで終わること (異常終了で通らないよう、終了コードではなくメッセージを見る)
    set_tests_properties(host_neptune_bad_link PROPERTIES PASS_REGULAR_EXPRESSION "neptune: bad link 'bogus'")
    set_tests_properties(host_neptune_bad_mac PROPERTIES PASS_REGULAR_EXPRESSION "neptune: bad MAC address 01:00:00:00:00:00")
    # MIDI ボード: つないで起動する。読めない SoundFont は、知らせて音源なしで続ける
    # (ROM がなければテストパターンになるので飛ばす)
    add_test(NAME host_midi COMMAND sh -c
      "test -f \"$HOME/.cache/pix68k/rom/IPLROM.DAT\" || exit 77; '$<TARGET_FILE:pix68k>' --no-config --frames 3 --no-sram --wav host_midi.wav --midi --midi-sf2 /nonexistent.sf2")
    set_tests_properties(host_midi PROPERTIES PASS_REGULAR_EXPRESSION "midi: cannot load the SoundFont /nonexistent.sf2"
      SKIP_RETURN_CODE 77 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    # MT-32 の音源: 読めない ROM (ないファイル、大きさの違うファイル) は、知らせて音源なしで続ける
    add_test(NAME host_midi_mt32 COMMAND sh -c
      "test -f \"$HOME/.cache/pix68k/rom/IPLROM.DAT\" || exit 77; '$<TARGET_FILE:pix68k>' --no-config --frames 3 --no-sram --wav host_midi_mt32.wav --midi-mt32-ctrl /nonexistent.rom --midi-mt32-pcm '${CMAKE_SOURCE_DIR}/LICENSE'")
    set_tests_properties(host_midi_mt32 PROPERTIES
      PASS_REGULAR_EXPRESSION "midi: cannot load the MT-32 ROMs /nonexistent.rom and .*LICENSE: the CONTROL ROM is not"
      SKIP_RETURN_CODE 77 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    # SC-55mkII の音源: ROM のないディレクトリは、知らせて音源なしで続ける
    add_test(NAME host_midi_sc55 COMMAND sh -c
      "test -f \"$HOME/.cache/pix68k/rom/IPLROM.DAT\" || exit 77; '$<TARGET_FILE:pix68k>' --no-config --frames 3 --no-sram --wav host_midi_sc55.wav --midi-sc55 '${CMAKE_SOURCE_DIR}/external'")
    set_tests_properties(host_midi_sc55 PROPERTIES
      PASS_REGULAR_EXPRESSION "midi: .*external: no rom1.bin \\(R15199858, 32768 bytes\\); MIDI is not played"
      SKIP_RETURN_CODE 77 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    # UART の出力 (ホスト版はファイルに書く): 電源を入れたときのリセットで、16 のチャネルの音を止めるバイト (80 バイト) を書く
    add_test(NAME host_midi_uart COMMAND sh -c
      "test -f \"$HOME/.cache/pix68k/rom/IPLROM.DAT\" || exit 77; '$<TARGET_FILE:pix68k>' --no-config --frames 3 --no-sram --no-audio --midi --midi-out uart --midi-dump host_midi_uart.bin && echo \"dump size $(wc -c < host_midi_uart.bin | tr -d ' ')\"")
    set_tests_properties(host_midi_uart PROPERTIES PASS_REGULAR_EXPRESSION "MIDI: UART bytes to host_midi_uart.bin \\(gs\\).*dump size 80"
      SKIP_RETURN_CODE 77 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    add_test(NAME host_neptune_eth COMMAND pix68k --no-config --frames 3 --no-sram --no-audio --neptune-link eth)
    set_tests_properties(host_neptune_eth PROPERTIES PASS_REGULAR_EXPRESSION "link eth is for the bare metal build; using echo"
      ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    set_tests_properties(host_smoke host_smoke_pattern host_config_empty_values host_neptune host_neptune_bad_link
      host_neptune_bad_mac PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    # SRAM のファイル: 大きさの違うファイルを無視すること、一時ファイルと rename で書き戻すこと、次の起動で読むこと
    add_test(NAME host_sram COMMAND sh ${CMAKE_SOURCE_DIR}/tests/host/sram_test.sh $<TARGET_FILE:pix68k>)
    set_tests_properties(host_sram PROPERTIES SKIP_RETURN_CODE 77)
    # --time: Human68k の time コマンドが決めた時刻を出し、同じ時刻なら画面も WAV も同じになる (#149)
    add_test(NAME host_time COMMAND sh ${CMAKE_SOURCE_DIR}/tests/host/time_test.sh $<TARGET_FILE:pix68k> ${HUMAN68K_DISK})
    set_tests_properties(host_time PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    # --time の値の検査 (ROM なしで動く)
    add_test(NAME host_time_args COMMAND sh ${CMAKE_SOURCE_DIR}/tests/host/time_args_test.sh $<TARGET_FILE:pix68k> 0 "showing the test pattern" --frames 1 --no-sram --no-audio)
    # --profile: ほかのオプションの値 (--type "--profile") を拾わない。組み合わせのファイルがなければ終了コード 2 (#231)
    add_test(NAME host_profile_args COMMAND sh ${CMAKE_SOURCE_DIR}/tests/host/profile_args_test.sh $<TARGET_FILE:pix68k>)
    add_test(NAME host_menu COMMAND sh ${CMAKE_SOURCE_DIR}/tests/host/menu_test.sh $<TARGET_FILE:pix68k>)
    set_tests_properties(host_menu PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    # CUE シートの CD-ROM のイメージをつなぎ、メニューで媒体を入れ替える (X68030 の IPL ROM がなければ飛ばす)
    add_test(NAME host_cdrom COMMAND sh ${CMAKE_SOURCE_DIR}/tests/host/cdrom_test.sh $<TARGET_FILE:pix68k> ${X68030_IPLROM})
    set_tests_properties(host_cdrom PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
  else()
    message(STATUS "SDL2 が見つからないので、ホスト版 (pix68k) はビルドしない")
  endif()
endif()
