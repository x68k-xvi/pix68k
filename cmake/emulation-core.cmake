set(PIX68K_CORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
# Called from the project root (full build or portable libretro build).
add_library(m68k STATIC ${PIX68K_CORE_ROOT}/src/m68k/m68k.cpp)
if(PIX68K_BUILD_CPU020)
  target_sources(m68k PRIVATE ${PIX68K_CORE_ROOT}/src/m68k/cpu020.cpp ${PIX68K_CORE_ROOT}/src/m68k/fpu.cpp)
else()
  target_compile_definitions(m68k PUBLIC PIX68K_NO_CPU020)
endif()
if(PIX68K_BUILD_CPU020 OR PIX68K_BUILD_FPU_BOARD)
  target_sources(m68k PRIVATE ${PIX68K_CORE_ROOT}/src/m68k/fpu_core.cpp ${PIX68K_CORE_ROOT}/src/m68k/fpu_math.cpp)
endif()
target_include_directories(m68k PUBLIC "${PIX68K_CORE_ROOT}/src")
target_compile_options(m68k PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)

add_library(x68k STATIC ${PIX68K_CORE_ROOT}/src/x68k/machine.cpp ${PIX68K_CORE_ROOT}/src/x68k/mfp.cpp ${PIX68K_CORE_ROOT}/src/x68k/rtc.cpp ${PIX68K_CORE_ROOT}/src/x68k/video.cpp ${PIX68K_CORE_ROOT}/src/x68k/devices.cpp
  ${PIX68K_CORE_ROOT}/src/x68k/gvram.cpp ${PIX68K_CORE_ROOT}/src/x68k/render.cpp
  ${PIX68K_CORE_ROOT}/src/x68k/fdd.cpp ${PIX68K_CORE_ROOT}/src/x68k/fdc.cpp ${PIX68K_CORE_ROOT}/src/x68k/dmac.cpp ${PIX68K_CORE_ROOT}/src/x68k/keyboard.cpp ${PIX68K_CORE_ROOT}/src/x68k/textscreen.cpp ${PIX68K_CORE_ROOT}/src/x68k/sram_file.cpp
  ${PIX68K_CORE_ROOT}/src/x68k/scc.cpp ${PIX68K_CORE_ROOT}/src/x68k/mouse.cpp ${PIX68K_CORE_ROOT}/src/x68k/ppi.cpp ${PIX68K_CORE_ROOT}/src/x68k/sasi.cpp ${PIX68K_CORE_ROOT}/src/x68k/spc.cpp ${PIX68K_CORE_ROOT}/src/x68k/cd_drive.cpp
  ${PIX68K_CORE_ROOT}/src/x68k/opm.cpp ${PIX68K_CORE_ROOT}/src/x68k/adpcm.cpp ${PIX68K_CORE_ROOT}/src/x68k/sound.cpp ${PIX68K_CORE_ROOT}/src/x68k/video_replay.cpp ${PIX68K_CORE_ROOT}/src/x68k/sound_replay.cpp
  ${PIX68K_CORE_ROOT}/src/x68k/ts6bga.cpp ${PIX68K_CORE_ROOT}/src/x68k/state.cpp ${PIX68K_CORE_ROOT}/src/x68k/mercury.cpp ${PIX68K_CORE_ROOT}/src/x68k/neptune.cpp ${PIX68K_CORE_ROOT}/src/x68k/ym3802.cpp)
if(PIX68K_BUILD_FPU_BOARD)
  target_sources(x68k PRIVATE ${PIX68K_CORE_ROOT}/src/x68k/fpu_board.cpp)
else()
  target_compile_definitions(x68k PUBLIC PIX68K_NO_FPU_BOARD)
endif()
target_include_directories(x68k PUBLIC "${PIX68K_CORE_ROOT}/src")
# -fcheck-new: new が 0 を返す環境 (Circle) で、確保の失敗を Machine::ok() で返すため
target_compile_options(x68k PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti -fcheck-new)
target_link_libraries(x68k m68k)

