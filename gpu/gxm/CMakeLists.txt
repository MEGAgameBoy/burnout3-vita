# The Vita's GPU (GXM): the device (gxm_device.cpp) and the display
# (gxm_display.cpp, a ps2gpu::Display) - the Vita's counterpart of
# engine/gpu's Vulkan code. Cg compiled on the Vita by libshacccg, which the
# program loads at run time: its imports are weak, so the program starts
# without it (and shows the CPU's picture instead).

add_library(ps2gxm STATIC
    ${CMAKE_CURRENT_LIST_DIR}/gxm_device.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gxm_display.cpp)
target_include_directories(ps2gxm PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include)
target_compile_features(ps2gxm PUBLIC cxx_std_17)
target_link_libraries(ps2gxm PUBLIC ps2plat SceGxm_stub SceDisplay_stub SceShaccCg_stub_weak SceKernelModulemgr_stub)
