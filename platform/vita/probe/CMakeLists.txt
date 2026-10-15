# The Vita's code-size probes: an executable of each size in PROBE_SIZES
# (MB of code), as VPKs - whether the system loads executables the size of a
# translated game.
#
#   cmake -S engine/platform/vita/probe -B build/probe -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
#   cmake --build build/probe

cmake_minimum_required(VERSION 3.20)
project(ps2plat_probe CXX ASM)

set(PROBE_SIZES 32 64 96 CACHE STRING "MB of code, one probe each")
set(PS2_ENGINE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../..)
add_subdirectory(${PS2_ENGINE_DIR}/platform ${CMAKE_BINARY_DIR}/ps2plat)
include(${PS2_ENGINE_DIR}/cmake/vita.cmake)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

foreach(size IN LISTS PROBE_SIZES)
    set(name ps2plat_probe${size})
    set(dir ${CMAKE_CURRENT_BINARY_DIR}/${name}_generated)
    add_custom_command(OUTPUT ${dir}/probe.S ${dir}/probe.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${dir}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/make_probe.py ${size} ${dir}/probe.S ${dir}/probe.h
        DEPENDS ${CMAKE_CURRENT_LIST_DIR}/make_probe.py
        COMMENT "probe: ${size} MB of code")
    add_executable(${name} ${CMAKE_CURRENT_LIST_DIR}/probe_main.cpp ${dir}/probe.S)
    target_include_directories(${name} PRIVATE ${dir})
    target_compile_definitions(${name} PRIVATE PROBE_NAME="${name}")
    target_link_libraries(${name} PRIVATE ps2plat)
    if(size GREATER 64)
        ps2_vita_package(${name} TITLE "Code probe ${size} MB" TITLE_ID PTEN00${size}0 VERSION 01.00 EXTENDED_MEMORY)
    else()
        ps2_vita_package(${name} TITLE "Code probe ${size} MB" TITLE_ID PTEN00${size}0 VERSION 01.00)
    endif()
endforeach()
