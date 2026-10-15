# The platform layer (ps2plat): fibers, the host's threads, sleeps, files,
# paths, the terminal and messages - one implementation a system.

if(WIN32)
    add_library(ps2plat STATIC ${CMAKE_CURRENT_LIST_DIR}/src/win32.cpp)
elseif(VITA)
    enable_language(ASM)
    add_library(ps2plat STATIC
        ${CMAKE_CURRENT_LIST_DIR}/src/vita.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/fibers.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/fiber_arm.S)
    target_link_libraries(ps2plat PUBLIC
        SceAudio_stub SceCtrl_stub SceTouch_stub ScePower_stub SceAppUtil_stub SceAppMgr_stub
        SceSysmodule_stub SceDisplay_stub SceCommonDialog_stub SceLibKernel_stub pthread)
else()
    set(ps2plat_sources
        ${CMAKE_CURRENT_LIST_DIR}/src/posix.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/fibers.cpp)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|ARM)")
        enable_language(ASM)
        list(APPEND ps2plat_sources ${CMAKE_CURRENT_LIST_DIR}/src/fiber_arm.S)
    endif()
    add_library(ps2plat STATIC ${ps2plat_sources})
    find_package(Threads REQUIRED)
    target_link_libraries(ps2plat PUBLIC Threads::Threads)
endif()
target_include_directories(ps2plat PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_features(ps2plat PUBLIC cxx_std_17)
