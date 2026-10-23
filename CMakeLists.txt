cmake_minimum_required(VERSION 3.20)
project(daytona_recomp LANGUAGES C CXX)

if(ANDROID)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DAYTONA_N64_LOW_MEMORY "Build the low-memory N64 runtime path" OFF)
option(DAYTONA_N64_HALF_RESOLUTION "Use a 160x120 internal N64 framebuffer" OFF)
option(DAYTONA_N64_TEXTURE_10 "Use 10% of each Model 2 texture RAM sheet" OFF)
option(DAYTONA_N64_TEXTURE_25 "Use 25% of each Model 2 texture RAM sheet" OFF)
if(DAYTONA_N64_TEXTURE_10 AND DAYTONA_N64_TEXTURE_25)
    message(FATAL_ERROR "Select only one reduced N64 texture RAM mode")
endif()
if(DAYTONA_N64_LOW_MEMORY)
    add_compile_definitions(DAYTONA_N64_LOW_MEMORY DAYTONA_SERIAL_RENDER)
    if(DAYTONA_N64_HALF_RESOLUTION)
        add_compile_definitions(DAYTONA_N64_HALF_RESOLUTION)
    endif()
    if(DAYTONA_N64_TEXTURE_10)
        add_compile_definitions(DAYTONA_N64_TEXTURE_10)
    elseif(DAYTONA_N64_TEXTURE_25)
        add_compile_definitions(DAYTONA_N64_TEXTURE_25)
    endif()
endif()

if(MSVC)
    # /bigobj: the generated game code has many labels per object file
    add_compile_options(/W4 /EHsc /fp:strict /bigobj /utf-8)
else()
    # No -ffast-math, ever (rules.md): exact rounding is the point. No FP
    # contraction either: a fused multiply-add (ARM64 always has one) rounds
    # once where the TGP's code rounds twice.
    add_compile_options(-Wall -Wextra -fno-fast-math -ffp-contract=off)
endif()

# Berkeley SoftFloat 3e (scripts/fetch_softfloat.sh): the extF80 reference
# model for i960 FP. 8086-SSE specialisation, FAST_INT64 build.
set(SOFTFLOAT_DIR ${CMAKE_SOURCE_DIR}/extern/softfloat)
include(cmake/softfloat_sources.cmake)
add_library(softfloat STATIC ${SOFTFLOAT_SOURCES})
target_include_directories(softfloat
    PUBLIC ${SOFTFLOAT_DIR}/source/include
    PRIVATE ${CMAKE_SOURCE_DIR}/cmake/softfloat-platform ${SOFTFLOAT_DIR}/source/8086-SSE)
target_compile_definitions(softfloat PRIVATE
    SOFTFLOAT_ROUND_ODD INLINE_LEVEL=5 SOFTFLOAT_FAST_DIV32TO16 SOFTFLOAT_FAST_DIV64TO32)
# FAST_INT64 selects the by-value extF80 API and LITTLEENDIAN the field order of
# extFloat80_t in the public headers, so users must see both (a mismatch silently
# swaps signExp and signif across the call boundary). Little-endian hosts only.
# Rounding mode and flags are globals in SoftFloat: make them per thread.
# C++ users get __thread, not thread_local: on macOS an extern thread_local is
# reached through a C++ wrapper function the C definitions never emit.
target_compile_definitions(softfloat PUBLIC SOFTFLOAT_FAST_INT64 LITTLEENDIAN=1
    $<$<COMPILE_LANGUAGE:C>:THREAD_LOCAL=_Thread_local>
    $<$<COMPILE_LANGUAGE:CXX>:THREAD_LOCAL=$<IF:$<CXX_COMPILER_ID:MSVC>,thread_local,__thread>>)
set_target_properties(softfloat PROPERTIES C_STANDARD 11)
if(NOT MSVC)
    target_compile_options(softfloat PRIVATE -w) # upstream code, not ours
endif()

add_library(i960 STATIC
    src/i960/isa.cpp
    src/i960/decode.cpp
    src/i960/reach.cpp
    src/i960/fp.cpp
)
target_include_directories(i960 PUBLIC src)
target_link_libraries(i960 PUBLIC softfloat)

add_library(trace STATIC src/trace/trace.cpp)
target_include_directories(trace PUBLIC src)

# Runtime the recompiled game runs on: i960 context and services (call/ret,
# interrupts, memory; MAME's semantics, transplanted, BSD-3), replay bus and
# lockstep. No instruction interpreter.
add_library(runtime STATIC
    src/runtime/cpu.cpp
    src/runtime/tgp.cpp
    src/runtime/m2_tgp_board.cpp
    src/runtime/geo.cpp
    src/runtime/raster.cpp
    src/runtime/video.cpp
    src/runtime/presentation.cpp
    src/runtime/render_workers.cpp
    src/runtime/m2_board.cpp
    src/runtime/game_loop.cpp
    src/runtime/zip.cpp
    src/runtime/rom_import.cpp
    src/runtime/archive.cpp
    src/runtime/m2_replay_bus.cpp
    src/runtime/lockstep.cpp
    src/runtime/snd_cpu.cpp
    src/runtime/multipcm.cpp
    src/runtime/sound_board.cpp
)
target_include_directories(runtime PUBLIC src)
# 7z ROM sets: the 7-Zip LZMA SDK's decoder (public-domain files only),
# fetched by scripts/setup.py into extern/lzma.
set(LZMA_DIR ${CMAKE_SOURCE_DIR}/extern/lzma/C)
if(EXISTS ${LZMA_DIR}/7zArcIn.c)
    add_library(lzma7z STATIC
        ${LZMA_DIR}/7zArcIn.c ${LZMA_DIR}/7zDec.c ${LZMA_DIR}/7zBuf.c ${LZMA_DIR}/7zCrc.c ${LZMA_DIR}/7zCrcOpt.c
        ${LZMA_DIR}/7zStream.c ${LZMA_DIR}/7zAlloc.c ${LZMA_DIR}/Alloc.c ${LZMA_DIR}/LzmaDec.c ${LZMA_DIR}/Lzma2Dec.c
        ${LZMA_DIR}/Bra.c ${LZMA_DIR}/Bra86.c ${LZMA_DIR}/BraIA64.c ${LZMA_DIR}/Bcj2.c ${LZMA_DIR}/Delta.c
        ${LZMA_DIR}/CpuArch.c ${LZMA_DIR}/Ppmd7.c ${LZMA_DIR}/Ppmd7Dec.c)
    target_include_directories(lzma7z SYSTEM PUBLIC ${LZMA_DIR})
    target_compile_definitions(lzma7z PRIVATE Z7_PPMD_SUPPORT) # 7z's PPMd method too, not only LZMA/LZMA2
    if(NOT MSVC)
        target_compile_options(lzma7z PRIVATE -w)
    endif()
    target_link_libraries(runtime PUBLIC lzma7z)
    target_compile_definitions(runtime PRIVATE M2_HAVE_7Z)
else()
    message(STATUS "extern/lzma not found: 7z ROM sets unsupported (run scripts/setup.py)")
endif()
target_link_libraries(runtime PUBLIC trace i960)

# YM3438 (the sound board's FM chip): ymfm, BSD-3, fetched by
# scripts/setup.py into extern/ymfm.
set(YMFM_DIR ${CMAKE_SOURCE_DIR}/extern/ymfm/src)
if(NOT EXISTS ${YMFM_DIR}/ymfm_opn.cpp)
    message(FATAL_ERROR "extern/ymfm not found: run scripts/setup.py")
endif()
add_library(ymfm STATIC ${YMFM_DIR}/ymfm_opn.cpp ${YMFM_DIR}/ymfm_adpcm.cpp ${YMFM_DIR}/ymfm_ssg.cpp)
target_include_directories(ymfm SYSTEM PUBLIC ${YMFM_DIR})
if(NOT MSVC)
    target_compile_options(ymfm PRIVATE -w)
endif()
target_link_libraries(runtime PUBLIC ymfm)

# Reference interpreter (MAME's i960 executor, transplanted, BSD-3). Test
# oracle only: m2replay links it; the game never does.
add_library(refcore STATIC src/refcore/i960_ref.cpp)
target_link_libraries(refcore PUBLIC runtime)
if(NOT MSVC)
    # Upstream MAME code: keep its own warnings out of ours.
    set_source_files_properties(src/runtime/cpu.cpp src/refcore/i960_ref.cpp src/runtime/geo.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-sign-compare;-Wno-unused-but-set-variable;-Wno-attributes")
endif()

add_executable(m2replay tools/m2replay/main.cpp)
target_link_libraries(m2replay PRIVATE refcore)

# The recompiler (ours; committed) and, when it has been run, its output.
add_executable(m2recomp tools/m2recomp/main.cpp)
target_link_libraries(m2recomp PRIVATE i960)

# The sound CPU (68000) decoder and static recompiler.
add_library(m68k STATIC src/m68k/decode.cpp src/m68k/reach.cpp)
target_include_directories(m68k PUBLIC src)
add_executable(m2sndrecomp tools/m2sndrecomp/main.cpp)
target_link_libraries(m2sndrecomp PRIVATE m68k)

# Recompiled game code lives in build/gen/<set>/ (derived from the game: never
# committed). scripts/recompile.sh generates it and re-runs CMake.
set(DAYTONA_GENERATED_ROOT "${CMAKE_BINARY_DIR}/gen" CACHE PATH "Local ROM-derived generated sources")
set(GEN_DIR ${DAYTONA_GENERATED_ROOT}/daytona93)
set(TGP_GEN ${DAYTONA_GENERATED_ROOT}/daytona93_tgp/tgp_gen.cpp)
set(SND_GEN ${DAYTONA_GENERATED_ROOT}/daytona93_snd/snd_gen.cpp)
if(EXISTS ${GEN_DIR}/gen_table.cpp AND EXISTS ${TGP_GEN} AND EXISTS ${SND_GEN})
    file(GLOB GEN_SOURCES ${GEN_DIR}/*.cpp)
    set(DAYTONA_PREBUILT_GAME_CODE "" CACHE FILEPATH "Optional local cache of unchanged generated code")
    if(DAYTONA_PREBUILT_GAME_CODE)
        add_library(daytona_game_code STATIC IMPORTED)
        set_target_properties(daytona_game_code PROPERTIES IMPORTED_LOCATION "${DAYTONA_PREBUILT_GAME_CODE}")
        set_property(TARGET daytona_game_code PROPERTY INTERFACE_LINK_LIBRARIES runtime)
    else()
        add_library(daytona_game_code STATIC ${GEN_SOURCES} ${TGP_GEN} ${SND_GEN})
        target_link_libraries(daytona_game_code PUBLIC runtime)
    endif()
    add_executable(m2native tools/m2native/main.cpp)
    target_link_libraries(m2native PRIVATE daytona_game_code)
    # The game on its own: native board runtime, no trace.
    add_executable(m2run tools/m2run/main.cpp)
    target_link_libraries(m2run PRIVATE daytona_game_code)

    # The game in a window: SDL3 (window, input, SDL_GPU: Vulkan, Direct3D 12,
    # Metal). SDL is fetched by scripts/setup.py into extern/sdl3.
    if(NOT ANDROID)
        find_package(SDL3 CONFIG QUIET)
    endif()
    if(TARGET SDL3::SDL3 OR EXISTS ${CMAKE_SOURCE_DIR}/extern/sdl3/CMakeLists.txt)
        if(NOT TARGET SDL3::SDL3)
            if(ANDROID)
                set(SDL_SHARED ON CACHE BOOL "" FORCE)
                set(SDL_STATIC OFF CACHE BOOL "" FORCE)
            else()
                set(SDL_SHARED OFF CACHE BOOL "" FORCE)
                set(SDL_STATIC ON CACHE BOOL "" FORCE)
            endif()
            set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
            set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
            add_subdirectory(${CMAKE_SOURCE_DIR}/extern/sdl3 ${CMAKE_BINARY_DIR}/sdl3 EXCLUDE_FROM_ALL)
        endif()
        # Dear ImGui (launcher UI), with its SDL3 and SDL_GPU backends
        set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/extern/imgui)
        if(ANDROID)
            set(IMGUI_RENDERER ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
        else()
            set(IMGUI_RENDERER ${IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp)
        endif()
        add_library(imgui STATIC
            ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
            ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/imgui_demo.cpp
            ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_RENDERER})
        target_include_directories(imgui SYSTEM PUBLIC ${IMGUI_DIR})
        target_link_libraries(imgui PUBLIC SDL3::SDL3)
        if(NOT MSVC)
            target_compile_options(imgui PRIVATE -w)
        endif()
        if(ANDROID)
            add_library(main SHARED src/app/android_main.cpp src/app/launcher.cpp src/app/config.cpp src/app/controls.cpp src/app/touch_controls.cpp)
            target_link_libraries(main PRIVATE daytona_game_code imgui SDL3::SDL3)
            target_link_options(main PRIVATE "-Wl,-z,max-page-size=16384")
        else()
            add_executable(daytona WIN32 src/app/main.cpp src/app/launcher.cpp src/app/config.cpp src/app/controls.cpp src/app/gpu_geometry.cpp)
            target_link_libraries(daytona PRIVATE daytona_game_code imgui SDL3::SDL3)
            add_executable(check_gpu_geometry tools/check_gpu_geometry.cpp src/app/gpu_geometry.cpp)
            target_link_libraries(check_gpu_geometry PRIVATE daytona_game_code SDL3::SDL3)
        endif()
    else()
        message(STATUS "extern/sdl3 not found: skipping the windowed game (run scripts/setup.py)")
    endif()
    if(NOT MSVC)
        set_source_files_properties(${GEN_SOURCES} ${TGP_GEN} ${SND_GEN} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label")
    endif()
endif()

# The TGP recompiler and, when it has been run, the lockstep check of its
# output against MAME's TGP log.
# The ROM set importer (zip or 7z) for the build.
add_executable(m2import tools/m2import/main.cpp)
target_link_libraries(m2import PRIVATE runtime)

add_executable(m2tgprecomp tools/m2tgprecomp/main.cpp)
target_include_directories(m2tgprecomp PRIVATE src)
if(EXISTS ${TGP_GEN})
    add_executable(m2tgpcheck tools/m2tgpcheck/main.cpp ${TGP_GEN})
    target_link_libraries(m2tgpcheck PRIVATE runtime)
    # The per-instruction hook exists only in this test build.
    target_compile_definitions(m2tgpcheck PRIVATE M2TGP_WITH_HOOK)
    if(NOT MSVC)
        set_source_files_properties(${TGP_GEN} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label")
    endif()
endif()

# The lockstep check of the recompiled sound 68000 against MAME's log
# (M2TRACE_SNDLOG, patches/mame/0003).
if(EXISTS ${SND_GEN})
    add_executable(m2sndcheck tools/m2sndcheck/main.cpp ${SND_GEN})
    target_link_libraries(m2sndcheck PRIVATE runtime)
    if(NOT MSVC)
        set_source_files_properties(${SND_GEN} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label")
    endif()
endif()

add_executable(i960dis tools/i960dis/main.cpp)
target_link_libraries(i960dis PRIVATE i960)

add_executable(tracediff tools/tracediff/main.cpp)
target_link_libraries(tracediff PRIVATE trace)

add_executable(tracedev tools/tracedev/main.cpp)
target_link_libraries(tracedev PRIVATE trace)

add_executable(traceinfo tools/traceinfo/main.cpp)
target_link_libraries(traceinfo PRIVATE trace)

enable_testing()

add_executable(test_decode tests/test_decode.cpp)
target_link_libraries(test_decode PRIVATE i960)
add_test(NAME decode COMMAND test_decode)

add_executable(test_m68k tests/test_m68k.cpp)
target_link_libraries(test_m68k PRIVATE m68k runtime)
add_test(NAME m68k COMMAND test_m68k)

add_executable(test_fp tests/test_fp.cpp)
target_link_libraries(test_fp PRIVATE i960)
find_package(Threads REQUIRED)
target_link_libraries(runtime PUBLIC Threads::Threads)
target_link_libraries(test_fp PRIVATE Threads::Threads)
add_test(NAME fp COMMAND test_fp)

# Measurement, not a test: where MAME's double FP disagrees with the model.
add_executable(fp_vs_mame tests/fp_vs_mame.cpp)
target_link_libraries(fp_vs_mame PRIVATE i960 Threads::Threads)

add_executable(test_reach tests/test_reach.cpp)
target_link_libraries(test_reach PRIVATE i960)
add_test(NAME reach COMMAND test_reach)

add_executable(test_trace tests/test_trace.cpp)
target_link_libraries(test_trace PRIVATE trace)
add_test(NAME trace COMMAND test_trace ${CMAKE_CURRENT_BINARY_DIR})

# The MAME plugin's Lua core against the C++ trace library (needs lupa).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME lua_core
        COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/lua_core_test.py
                ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/lua_core)
    set_tests_properties(lua_core PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME lua_plugin_mock
        COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/lua_plugin_mock_test.py
                ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/lua_plugin_mock)
    set_tests_properties(lua_plugin_mock PROPERTIES SKIP_RETURN_CODE 77)
    # Second decode reference: Ghidra SLEIGH (third-party i960 module via
    # pypcode). Needs scripts/fetch_ghidra_i960.sh and pip install pypcode.
    if(EXISTS ${CMAKE_SOURCE_DIR}/extern/ghidra_i960/data/languages/i960.slaspec)
        add_test(NAME ghidra_oracle
            COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/ghidra_oracle.py
                    ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_SOURCE_DIR}/extern/ghidra_i960 --random 200000)
        set_tests_properties(ghidra_oracle PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

# Differential test against MAME's disassembler. Needs scripts/fetch_mame.sh.
set(MAME_DIR ${CMAKE_SOURCE_DIR}/extern/mame CACHE PATH "Pinned MAME checkout")
if(EXISTS ${MAME_DIR}/src/devices/cpu/i960/i960dis.cpp)
    add_executable(mame_oracle
        tests/mame_oracle.cpp
        ${MAME_DIR}/src/devices/cpu/i960/i960dis.cpp
    )
    target_include_directories(mame_oracle PRIVATE
        tests/mame_shim
        ${MAME_DIR}/src/devices
        ${MAME_DIR}/src/devices/cpu/i960
    )
    target_link_libraries(mame_oracle PRIVATE i960)
    find_package(Threads REQUIRED)
    target_link_libraries(mame_oracle PRIVATE Threads::Threads)
    # MAME's source, not ours: don't drown our warnings in its.
    set_source_files_properties(${MAME_DIR}/src/devices/cpu/i960/i960dis.cpp
        PROPERTIES COMPILE_OPTIONS "-w")
    add_test(NAME mame_oracle COMMAND mame_oracle)
else()
    message(STATUS "extern/mame not found: skipping mame_oracle (run scripts/fetch_mame.sh)")
endif()

add_executable(test_presentation tests/test_presentation.cpp)
target_link_libraries(test_presentation PRIVATE runtime)
add_test(NAME presentation COMMAND test_presentation)
add_executable(test_geo_batch tests/test_geo_batch.cpp)
target_link_libraries(test_geo_batch PRIVATE runtime)
add_test(NAME geo_batch COMMAND test_geo_batch)
if(TARGET SDL3::SDL3)
    add_executable(test_controls tests/test_controls.cpp src/app/controls.cpp)
    target_include_directories(test_controls PRIVATE src)
    target_link_libraries(test_controls PRIVATE SDL3::SDL3)
    add_test(NAME xbox_controls COMMAND test_controls)
endif()

if(TARGET imgui AND NOT ANDROID)
    add_executable(test_touch tests/test_touch.cpp src/app/touch_controls.cpp src/app/controls.cpp)
    target_include_directories(test_touch PRIVATE src)
    target_link_libraries(test_touch PRIVATE imgui SDL3::SDL3)
    add_test(NAME touch_controls COMMAND test_touch)
endif()


add_executable(test_3ds_controls tests/test_3ds_controls.cpp)
target_link_libraries(test_3ds_controls PRIVATE runtime)
add_test(NAME 3ds_controls COMMAND test_3ds_controls)

add_executable(test_3ds_framebuffer tests/test_3ds_framebuffer.cpp)
target_include_directories(test_3ds_framebuffer PRIVATE src)
add_test(NAME 3ds_framebuffer COMMAND test_3ds_framebuffer)

if(DAYTONA_N64_LOW_MEMORY)
    add_executable(test_n64_low_memory tests/test_n64_low_memory.cpp)
    target_link_libraries(test_n64_low_memory PRIVATE runtime)
    if(TARGET daytona_game_code)
        target_link_libraries(test_n64_low_memory PRIVATE daytona_game_code)
        target_compile_definitions(test_n64_low_memory PRIVATE DAYTONA_TEST_WITH_GAME_CODE)
    endif()
    add_test(NAME n64_low_memory COMMAND test_n64_low_memory)

    add_executable(test_n64_chunked_rom tests/test_n64_chunked_rom.cpp n64/chunked_rom.cpp)
    target_include_directories(test_n64_chunked_rom PRIVATE tests/n64_mock n64)
    add_test(NAME n64_chunked_rom COMMAND test_n64_chunked_rom)
endif()

add_executable(test_lockstep_callbacks tests/test_lockstep_callbacks.cpp)
target_link_libraries(test_lockstep_callbacks PRIVATE runtime)
add_test(NAME lockstep_callbacks COMMAND test_lockstep_callbacks)
