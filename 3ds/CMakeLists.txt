cmake_minimum_required(VERSION 3.20)
project(daytona_3ds LANGUAGES C CXX ASM)
option(DAYTONA_LTO "Optimize the complete native program at link time (requires replay validation)" OFF)
option(DAYTONA_3DS_PICA200 "Build the native Citro3D/PICA200 polygon renderer" OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(SDK "" CACHE PATH "Path to separately installed 3DS SDK and local generated game data")
if(NOT SDK)
  message(FATAL_ERROR "Set -DSDK to your separately installed 3DS SDK and generated game-data directory")
endif()
set(ARM_FLAGS -march=armv6k -mtune=mpcore -mfpu=vfp -mfloat-abi=hard)
foreach(CRT crti.o crtbegin.o crtend.o crtn.o)
  execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${ARM_FLAGS} -print-file-name=${CRT} OUTPUT_VARIABLE CRT_PATH OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  set(${CRT} "${CRT_PATH}")
endforeach()
add_compile_options(-O2 -ffunction-sections -fdata-sections -fno-fast-math -ffp-contract=off)
# Keep the expensive software renderers at a higher optimization level while
# retaining the smaller/faster-to-build -O2 setting for the remaining runtime.
set_source_files_properties(${ROOT}/src/runtime/raster.cpp ${ROOT}/src/runtime/video.cpp
  PROPERTIES COMPILE_OPTIONS "-O3")
include_directories(${ROOT}/src ${SDK}/game-generated ${SDK}/sources/libctru/libctru/include ${SDK}/sdk-local/arm-none-eabi/include)
# New Nintendo 3DS has multiple application cores. The raster worker pool is
# enabled for its native target; diagnostic parity checks verify exact pixels.
add_compile_definitions(__3DS__ DAYTONA_3DS_RGBA_FRAMEBUFFER)
set(SOFTFLOAT_DIR ${ROOT}/extern/softfloat)
include(${ROOT}/cmake/softfloat_sources.cmake)
add_library(softfloat STATIC ${SOFTFLOAT_SOURCES})
target_include_directories(softfloat PUBLIC ${SOFTFLOAT_DIR}/source/include PRIVATE ${ROOT}/cmake/softfloat-platform ${SOFTFLOAT_DIR}/source/8086-SSE)
target_compile_definitions(softfloat PRIVATE SOFTFLOAT_ROUND_ODD INLINE_LEVEL=5 SOFTFLOAT_FAST_DIV32TO16 SOFTFLOAT_FAST_DIV64TO32)
target_compile_definitions(softfloat PUBLIC SOFTFLOAT_FAST_INT64 LITTLEENDIAN=1 $<$<COMPILE_LANGUAGE:C>:THREAD_LOCAL=_Thread_local> $<$<COMPILE_LANGUAGE:CXX>:THREAD_LOCAL=__thread>)
set_target_properties(softfloat PROPERTIES C_STANDARD 11)
add_library(core STATIC
${ROOT}/src/runtime/cpu.cpp
${ROOT}/src/runtime/tgp.cpp
${ROOT}/src/runtime/m2_tgp_board.cpp
${ROOT}/src/runtime/geo.cpp
${ROOT}/src/runtime/raster.cpp
${ROOT}/src/runtime/video.cpp
${ROOT}/src/runtime/presentation.cpp
${ROOT}/src/runtime/render_workers.cpp
${ROOT}/src/runtime/m2_board.cpp
${ROOT}/src/runtime/game_loop.cpp
${ROOT}/src/runtime/zip.cpp
${ROOT}/src/runtime/lockstep.cpp
${ROOT}/src/runtime/snd_cpu.cpp
${ROOT}/src/runtime/multipcm.cpp
${ROOT}/src/runtime/sound_board.cpp
${ROOT}/src/i960/isa.cpp
${ROOT}/src/i960/decode.cpp
${ROOT}/src/i960/reach.cpp
${ROOT}/src/i960/fp.cpp
${ROOT}/src/trace/trace.cpp
${ROOT}/extern/ymfm/src/ymfm_opn.cpp
${ROOT}/extern/ymfm/src/ymfm_adpcm.cpp
${ROOT}/extern/ymfm/src/ymfm_ssg.cpp
)
target_include_directories(core PUBLIC ${ROOT}/extern/ymfm/src)
target_link_libraries(core PUBLIC softfloat)
if(DAYTONA_3DS_PICA200)
  # Citro3D is kept alongside the local 3DS SDK (like libctru), rather than
  # vendored into the game source tree. Keep the GPU backend optional until
  # the PICA200 path passes image-parity and frame-time validation.
  set(CITRO3D_ROOT "${SDK}/sources/citro3d" CACHE PATH "Citro3D source checkout")
  if(NOT EXISTS "${CITRO3D_ROOT}/include/citro3d.h")
    message(FATAL_ERROR "DAYTONA_3DS_PICA200 requires Citro3D at ${CITRO3D_ROOT}")
  endif()
  file(GLOB_RECURSE CITRO3D_SOURCES CONFIGURE_DEPENDS "${CITRO3D_ROOT}/source/*.c")
  list(FILTER CITRO3D_SOURCES EXCLUDE REGEX "/(uniforms|texture)[.]c$")
  list(APPEND CITRO3D_SOURCES "${ROOT}/3ds/uniforms_fast.c" "${ROOT}/3ds/textures_packed.c")
  set_source_files_properties("${ROOT}/3ds/uniforms_fast.c" "${ROOT}/3ds/textures_packed.c" PROPERTIES INCLUDE_DIRECTORIES "${CITRO3D_ROOT}/source")
  add_library(citro3d STATIC ${CITRO3D_SOURCES})
  target_include_directories(citro3d PUBLIC "${CITRO3D_ROOT}/include"
    "${SDK}/sources/libctru/libctru/include" "${SDK}/sdk-local/arm-none-eabi/include")
  target_compile_definitions(citro3d PRIVATE __3DS__ CITRO3D_BUILD NDEBUG=1)
  target_compile_options(citro3d PRIVATE -O2 -ffunction-sections -fdata-sections
    -fomit-frame-pointer -fno-math-errno)
endif()
file(GLOB GAME ${ROOT}/build/gen/daytona93/*.cpp)
add_library(sysbase STATIC
${SDK}/sources/newlib-3ds/libgloss/libsysbase/_exit.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/abort.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/assert.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/build_argv.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/chdir.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/chmod.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/clocks.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/concatenate.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/dirent.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/environ.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/execve.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/fchmod.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/flock.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/fnmatch.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/fork.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/fpathconf.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/fstat.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/fsync.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/ftruncate.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/getpid.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/getreent.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/gettod.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/handle_manager.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/iosupport.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/isatty.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/kill.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/link.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/lseek.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/lstat.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/malloc_vars.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/mkdir.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/pthread.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/nanosleep.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/open.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/pathconf.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/read.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/readlink.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/realpath.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/rename.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/rmdir.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/sbrk.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/scandir.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/sleep.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/stat.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/statvfs.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/symlink.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/syscall_support.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/times.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/truncate.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/unlink.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/usleep.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/utime.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/wait.c
${SDK}/sources/newlib-3ds/libgloss/libsysbase/write.c
)
target_compile_definitions(sysbase PRIVATE _BUILDING_LIBSYSBASE)
add_executable(daytona ${ROOT}/src/app/3ds_main.cpp ${GAME} ${ROOT}/build/gen/daytona93_tgp/tgp_gen.cpp ${ROOT}/build/gen/daytona93_snd/snd_gen.cpp ${SDK}/sources/devkitarm-crtls/3dsx_crt0.s)
target_link_directories(daytona PRIVATE ${SDK}/sdk-local/arm-none-eabi/lib)
target_link_options(daytona PRIVATE -nostartfiles -nodefaultlibs "-Wl,-T,${SDK}/sources/devkitarm-crtls/3dsx.ld,--gc-sections,--emit-relocs,--use-blx" "${crti.o}" "${crtbegin.o}")
if(DAYTONA_3DS_PICA200)
  target_sources(daytona PRIVATE ${ROOT}/src/app/3ds_gpu.cpp ${ROOT}/3ds/draw_arrays_packed.c ${ROOT}/3ds/draw_elements_packed.c ${ROOT}/3ds/frame_end_flushed.c)
  target_include_directories(daytona PRIVATE "${CITRO3D_ROOT}/source")
  target_compile_definitions(daytona PRIVATE DAYTONA_3DS_PICA200)
  target_link_libraries(daytona PRIVATE citro3d)
endif()
target_link_libraries(daytona PRIVATE "-Wl,--start-group" core "${SDK}/ctru-build/libctru.a" sysbase stdc++ m c gcc "-Wl,--end-group" "${crtend.o}" "${crtn.o}")
set_target_properties(daytona PROPERTIES SUFFIX .elf)
if(DAYTONA_LTO)
  set_property(TARGET softfloat core sysbase daytona PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
