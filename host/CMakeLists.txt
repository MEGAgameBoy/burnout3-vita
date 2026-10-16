# The native host: the shared runtime (PS2 instruction semantics, also used by
# verification) and the PC platform layer that serves the translated code's
# hooks. A game port adds its translated functions with ps2_add_port().

cmake_minimum_required(VERSION 3.20)

set(PS2_ENGINE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

add_library(ps2rt STATIC
    ${PS2_ENGINE_DIR}/runtime/src/eefpu.cpp
    ${PS2_ENGINE_DIR}/runtime/src/eeint.cpp
    ${PS2_ENGINE_DIR}/runtime/src/vu0.cpp
    ${PS2_ENGINE_DIR}/runtime/src/lifted_support.cpp)
target_include_directories(ps2rt PUBLIC ${PS2_ENGINE_DIR}/runtime/include)
target_compile_features(ps2rt PUBLIC cxx_std_17)

add_library(ps2host STATIC
    ${CMAKE_CURRENT_LIST_DIR}/src/host.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/memory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/kernel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/devices.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/ipu.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/vif1_worker.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/threads.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/iop.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/sdk.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/disc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/cdvd.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/fileio.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/vif.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/dma.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/loadfile.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/dbc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/mc2.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/mcserv.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/iop_vm.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/iop_libs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/iopheap.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/spu2.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/audio.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/vu.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/timers.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/pc_menu.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/window.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/settings.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/timeline.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/gs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/gs_thread.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/gs_draw.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/gs_memory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/gpu_bridge.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/profile.cpp)
# The IOP interpreter names library functions from engine/data/iop_exports.csv,
# built in (iop_exports.inc: a line of it a string), so that the executable
# reads nothing from where it was built.
file(STRINGS ${PS2_ENGINE_DIR}/data/iop_exports.csv iop_export_lines)
set(iop_exports_inc "// From engine/data/iop_exports.csv (engine/host/CMakeLists.txt): generated.\n")
foreach(line IN LISTS iop_export_lines)
    string(REPLACE "\\" "\\\\" line "${line}")
    string(REPLACE "\"" "\\\"" line "${line}")
    string(APPEND iop_exports_inc "\"${line}\\n\"\n")
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/iop_exports.inc.new "${iop_exports_inc}")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/generated/iop_exports.inc.new
               ${CMAKE_CURRENT_BINARY_DIR}/generated/iop_exports.inc COPYONLY)     # (touched only when it changes)
target_include_directories(ps2host PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PS2_ENGINE_DIR}/data/iop_exports.csv)
target_include_directories(ps2host PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(ps2host PUBLIC ps2rt)

# The platform layer (engine/platform): what differs from one system to another.
if(NOT TARGET ps2plat)
    add_subdirectory(${PS2_ENGINE_DIR}/platform ${CMAKE_BINARY_DIR}/ps2plat)
endif()
target_link_libraries(ps2host PUBLIC ps2plat)
# ps2rt's headers are inline in the generated code; ps2plat's CpuRelax too.
target_link_libraries(ps2rt PUBLIC ps2plat)

# Texture packs (texture_pack.cpp): single headers, pinned by hash - xxHash
# (BSD-2-Clause; PCSX2's names are XXH3 hashes), bcdec (MIT or Unlicense; DDS
# BC1-BC7) and stb_image (MIT or public domain; PNG).
set(third_party_dir ${CMAKE_CURRENT_BINARY_DIR}/third_party)
foreach(entry
        "xxhash.h|https://raw.githubusercontent.com/Cyan4973/xxHash/v0.8.3/xxhash.h|17973c0dc49d9854ca26caa191f0e12f7a424b68858d9a78de3860d959d85e4b"
        "bcdec.h|https://raw.githubusercontent.com/iOrange/bcdec/80859ed3b7afb1c527a2a99d70c61457bea72d0c/bcdec.h|134520764d96f70a27db814173616c89ad85db95fbd3328417c7a8c77e7ca189"
        "stb_image.h|https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image.h|594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3")
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 name)
    list(GET parts 1 url)
    list(GET parts 2 sha)
    if(NOT EXISTS ${third_party_dir}/${name})
        file(DOWNLOAD ${url} ${third_party_dir}/${name} EXPECTED_HASH SHA256=${sha} TLS_VERIFY ON)
    endif()
endforeach()
target_include_directories(ps2host PRIVATE ${third_party_dir})
target_sources(ps2host PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/texture_pack.cpp)

# The window through SDL3 and Vulkan (engine/gpu), with GDI as the fallback:
# Windows. (Elsewhere for now: no window - Linux runs headless for the tests;
# the Vita's display is its own, GXM.)
if(WIN32)
    set(ps2host_vulkan_default ON)
else()
    set(ps2host_vulkan_default OFF)
endif()
option(PS2HOST_VULKAN "Present the picture through SDL3 and Vulkan" ${ps2host_vulkan_default})
if(PS2HOST_VULKAN)
    add_subdirectory(${PS2_ENGINE_DIR}/gpu ${CMAKE_BINARY_DIR}/ps2gpu)
    target_link_libraries(ps2host PUBLIC ps2gpu)
    target_compile_definitions(ps2host PRIVATE PS2HOST_VULKAN=1)
endif()
# The Vita: its display through GXM (engine/gpu/gxm). PRIVATE: what links
# ps2host - the game's generated code - compiles without its headers (a
# change of flags would rebuild all of it).
if(VITA)
    add_subdirectory(${PS2_ENGINE_DIR}/gpu/gxm ${CMAKE_BINARY_DIR}/ps2gxm)
    target_link_libraries(ps2host PRIVATE ps2gxm)
    target_compile_definitions(ps2host PRIVATE PS2HOST_GXM=1)
endif()

if(MSVC)
    # Translated code throws lifted::Abort; its chunks are large.
    # AVX2: three-operand SIMD for the VU's lanes and the rest (6% of a race;
    # main.cpp says so on a processor without it).
    target_compile_options(ps2rt PUBLIC /EHsc /bigobj /W3 /wd4100 /wd4189 /arch:AVX2)
    target_compile_definitions(ps2rt PUBLIC _CRT_SECURE_NO_WARNINGS)
    target_compile_options(ps2host PRIVATE /Z7)     # symbols for --sample, as the ports'
    target_compile_options(ps2rt PRIVATE /Z7)
    target_link_libraries(ps2host PRIVATE dbghelp winmm)  # ...which resolves them; 1 ms sleeps
else()
    # GCC and Clang (Linux, the Vita). The translated code was verified as
    # MSVC compiles it, which neither assumes strict aliasing nor exploits a
    # signed overflow: the same here. No fused or contracted float
    # arithmetic: the EE's and the VUs' results are rounded step by step.
    target_compile_options(ps2rt PUBLIC -fno-strict-aliasing -fwrapv -ffp-contract=off)
endif()
if(PS2HOST_ARM_FLAGS)
    # ARMv7-A with NEON (the Vita's Cortex-A9; ARM Linux for the checks under
    # qemu): VU lanes in NEON for their integer half, VFP for the floats
    # (vu_math.h), FPSCR's rounding as the VUs round (vu.cpp, vu0.cpp).
    target_compile_options(ps2rt PUBLIC ${PS2HOST_ARM_FLAGS})
endif()

# The host's own tests, which need no game.
# vu_corpus.cpp: random microprograms, compiled by engine/tools/vurecomp.py
# --corpus as the build goes, which VuCompiledIsTheInterpreter runs both ways.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(PS2HOST_VURECOMP ${CMAKE_CURRENT_LIST_DIR}/../tools/vurecomp.py)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vu_corpus.cpp
    COMMAND ${Python3_EXECUTABLE} ${PS2HOST_VURECOMP} --corpus --count 120 --seed 1
            --out ${CMAKE_CURRENT_BINARY_DIR}/vu_corpus.cpp
    DEPENDS ${PS2HOST_VURECOMP}
    COMMENT "vurecomp: the VU test corpus")
add_executable(ps2host_tests ${CMAKE_CURRENT_LIST_DIR}/tests/host_tests.cpp
                             ${CMAKE_CURRENT_BINARY_DIR}/vu_corpus.cpp)
target_link_libraries(ps2host_tests PRIVATE ps2host)
if(PS2HOST_VULKAN)
    target_compile_definitions(ps2host_tests PRIVATE PS2HOST_VULKAN=1)
endif()
# The IOP interpreter's internals, for its instruction tests.
target_include_directories(ps2host_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
# On the Vita the tests are an application of their own (ps2host_tests.vpk):
# results on the screen and in ux0:data/ps2host_tests/results.txt.
include(${PS2_ENGINE_DIR}/cmake/vita.cmake)
if(VITA)
    target_sources(ps2host_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/tests/vita_main.cpp)
    target_compile_definitions(ps2host_tests PRIVATE PS2HOST_TESTS_NO_MAIN=1)
    ps2_vita_package(ps2host_tests TITLE "PS2 port engine tests" TITLE_ID PTEN00001 VERSION 01.00
                     SCE_SYS ${PS2_ENGINE_DIR}/platform/vita/tests_sce_sys)
endif()

# ps2_add_port(<target>
#     GENERATED <dir with dispatch.cpp from engine/tools/port.py>
#     TRANSLATED <dir with the game's translated sources (src/lifted)>
#     SOURCES <the game's own sources: overrides, enhancements>
#     GAME <the game's directory, with game.json>)
# With GAME, the port started without arguments (from Explorer, say) plays the
# game from the player's own disc image: asked for the first time (and checked
# to be the disc game.json's executable comes from), remembered after. And the
# executable carries the game's icon (the window's and the taskbar's too): the
# port's own, port/icon.ico - or build/port/icon.ico where a player made one
# for their own build (from their disc: a game's port/tools/make_icon.py). The
# game's project needs the RC language for it.
#
# On the Vita (VitaSDK's toolchain file) the port is also packaged as
# <target>.vpk: VITA_TITLE "<the name under the bubble>", VITA_TITLE_ID
# <ABCD12345>, VITA_SCE_SYS <the LiveArea's files>, VITA_HEAP_MB <newlib's
# heap>, and VITA_EXTENDED_MEMORY for the 365 MB budget (param.sfo's
# ATTRIBUTE2=12) instead of 256.
function(ps2_add_port target)
    cmake_parse_arguments(PORT "VITA_EXTENDED_MEMORY"
                          "GENERATED;TRANSLATED;GAME;VITA_TITLE;VITA_TITLE_ID;VITA_SCE_SYS;VITA_HEAP_MB"
                          "SOURCES" ${ARGN})
    file(GLOB translated ${PORT_TRANSLATED}/*.cpp)
    # dispatch.cpp, and discovered.cpp: code found copied at run time, translated.
    file(GLOB generated ${PORT_GENERATED}/*.cpp)
    add_executable(${target}
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/main.cpp
        ${generated}
        ${translated}
        ${PORT_SOURCES})
    target_link_libraries(${target} PRIVATE ps2host)
    # The application's name: its data directory on the Vita (ux0:data/<it>),
    # its log's name.
    target_compile_definitions(${target} PRIVATE PS2HOST_APP_NAME="${target}")
    if(PORT_GAME)
        get_filename_component(game_dir ${PORT_GAME} ABSOLUTE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${game_dir}/game.json)
        file(READ ${game_dir}/game.json game_json)
        string(JSON game_name GET ${game_json} name)
        string(JSON game_executable GET ${game_json} executable)
        string(JSON game_title ERROR_VARIABLE no_title GET ${game_json} title)
        if(no_title)
            set(game_title ${game_name})
        endif()
        string(JSON game_version ERROR_VARIABLE no_version GET ${game_json} version)
        if(no_version)
            set(game_version "0.0.0")
        endif()
        target_compile_definitions(${target} PRIVATE PS2HOST_GAME_NAME="${game_name}"
                                                     PS2HOST_GAME_TITLE="${game_title}"
                                                     PS2HOST_GAME_VERSION="${game_version}"
                                                     PS2HOST_GAME_EXECUTABLE="${game_executable}")
        if(WIN32)
            target_link_libraries(${target} PRIVATE comdlg32)
        endif()
        set(icon ${game_dir}/build/port/icon.ico)
        if(NOT EXISTS ${icon})
            set(icon ${game_dir}/port/icon.ico)
        endif()
        if(WIN32 AND EXISTS ${icon} AND CMAKE_RC_COMPILER)
            # The icon, and the version Explorer shows (game.json's "version",
            # its numbers for the fixed fields: 0.1.0-alpha is 0,1,0,0).
            string(REGEX MATCHALL "[0-9]+" version_numbers "${game_version}")
            list(APPEND version_numbers 0 0 0 0)
            list(SUBLIST version_numbers 0 4 version_numbers)
            string(REPLACE ";" "," version_fixed "${version_numbers}")
            file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target}_icon.rc
                 CONTENT "1 ICON \"${icon}\"
1 VERSIONINFO
FILEVERSION ${version_fixed}
PRODUCTVERSION ${version_fixed}
BEGIN
  BLOCK \"StringFileInfo\"
  BEGIN
    BLOCK \"040904B0\"
    BEGIN
      VALUE \"FileDescription\", \"${game_title} - PC port\"
      VALUE \"ProductName\", \"${game_title} - PC port\"
      VALUE \"FileVersion\", \"${game_version}\"
      VALUE \"ProductVersion\", \"${game_version}\"
      VALUE \"OriginalFilename\", \"${target}.exe\"
    END
  END
  BLOCK \"VarFileInfo\"
  BEGIN
    VALUE \"Translation\", 0x409, 1200
  END
END
")
            target_sources(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${target}_icon.rc)
            set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/${target}_icon.rc PROPERTIES
                                        OBJECT_DEPENDS ${icon})
        endif()
        if(MSVC)
            # A windowed program: started from Explorer, no console beside the
            # game (main.cpp attaches to a terminal's when there is one).
            target_link_options(${target} PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
        endif()
    endif()
    if(MSVC)
        # Symbols in the optimised build too, for --sample (profile.cpp): /Z7
        # keeps them in the objects (no PDB contention in parallel builds),
        # /DEBUG gathers them; /OPT keeps what it does without symbols.
        target_compile_options(${target} PRIVATE /Z7)
        # The symbols' file named by its name alone in the executable, not the
        # path it was built at (a release carries no one's folders).
        target_link_options(${target} PRIVATE /DEBUG /OPT:REF /OPT:ICF "/PDBALTPATH:%_PDB%")
    endif()
    if(VITA)
        # A section a function: the linker puts its long-branch veneers between
        # input sections, and a translated game's code is far past a Thumb
        # call's 16 MB reach (engine/platform/vita/probe found a single section
        # cannot be carried). -O2 rather than the toolchain's -O3 for the
        # translated code: GCC takes many times as long at -O3 on generated
        # sources (VITA_PLAN.md, "Code size, compile time, link").
        target_compile_options(${target} PRIVATE -ffunction-sections)
        set_source_files_properties(${translated} ${generated} TARGET_DIRECTORY ${target}
                                    PROPERTIES COMPILE_OPTIONS "-O2")
        if(PORT_VITA_HEAP_MB)
            target_compile_definitions(${target} PRIVATE PS2HOST_VITA_HEAP_MB=${PORT_VITA_HEAP_MB})
        endif()
        if(NOT PORT_VITA_TITLE)
            set(PORT_VITA_TITLE "${target}")
            if(game_title)
                set(PORT_VITA_TITLE "${game_title}")
            endif()
        endif()
        if(NOT PORT_VITA_TITLE_ID)
            message(FATAL_ERROR "ps2_add_port(${target}): VITA_TITLE_ID is needed on the Vita")
        endif()
        set(vita_args TITLE "${PORT_VITA_TITLE}" TITLE_ID ${PORT_VITA_TITLE_ID} VERSION 01.00)
        if(PORT_VITA_SCE_SYS)
            list(APPEND vita_args SCE_SYS ${PORT_VITA_SCE_SYS})
        endif()
        if(PORT_VITA_EXTENDED_MEMORY)
            list(APPEND vita_args EXTENDED_MEMORY)
        endif()
        ps2_vita_package(${target} ${vita_args})
    endif()
endfunction()
