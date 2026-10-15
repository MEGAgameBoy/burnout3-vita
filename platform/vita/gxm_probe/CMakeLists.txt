# The Vita's GPU probe (gxm_probe.cpp): libshacccg at run time, a scene read
# back, fragment programs reading the pixel they replace, integers in a
# fragment program - what the GXM GS renderer rests on.
#
#   cmake -S engine/platform/vita/gxm_probe -B build/gxm_probe -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
#   cmake --build build/gxm_probe         # ps2plat_gxm_probe.vpk

cmake_minimum_required(VERSION 3.20)
project(ps2plat_gxm_probe CXX ASM)

set(PS2_ENGINE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../..)
add_subdirectory(${PS2_ENGINE_DIR}/platform ${CMAKE_BINARY_DIR}/ps2plat)
include(${PS2_ENGINE_DIR}/cmake/vita.cmake)

add_executable(ps2plat_gxm_probe ${CMAKE_CURRENT_LIST_DIR}/gxm_probe.cpp)
# libshacccg's imports weak: the probe starts without it and says so.
target_link_libraries(ps2plat_gxm_probe PRIVATE ps2plat SceGxm_stub SceShaccCg_stub_weak SceKernelModulemgr_stub)
ps2_vita_package(ps2plat_gxm_probe TITLE "GXM probe 3" TITLE_ID PTEN00004 VERSION 01.00
                 SCE_SYS ${PS2_ENGINE_DIR}/platform/vita/tests_sce_sys)
