# SPDX-License-Identifier: GPL-2.0-or-later
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
find_program(CMAKE_OBJCOPY arm-none-eabi-objcopy REQUIRED)
find_program(CMAKE_SIZE arm-none-eabi-size REQUIRED)
find_program(CMAKE_NM arm-none-eabi-nm REQUIRED)

set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-m0 -mthumb -mfloat-abi=soft")
set(CMAKE_ASM_FLAGS_INIT "-mcpu=cortex-m0 -mthumb -mfloat-abi=soft")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
