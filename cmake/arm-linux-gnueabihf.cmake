# The engine for ARMv7 Linux, as the Vita's processor (a Cortex-A9 with NEON,
# hard-float): to run the host's tests under qemu-arm on a PC, the same code
# paths as the Vita's but its kernel. Ubuntu: g++-arm-linux-gnueabihf and
# qemu-user.
#
#   cmake -S engine/host -B build/arm -G Ninja -DCMAKE_TOOLCHAIN_FILE=engine/cmake/arm-linux-gnueabihf.cmake
#   qemu-arm -L /usr/arm-linux-gnueabihf build/arm/ps2host_tests

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_ASM_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-arm -L /usr/arm-linux-gnueabihf)
set(PS2HOST_ARM_FLAGS -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard CACHE STRING "")
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard")
