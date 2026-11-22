include(FetchContent)

# Serialize the first download when several build directories are configured.
file(MAKE_DIRECTORY "${PROJECT_SOURCE_DIR}/deps")
file(LOCK "${PROJECT_SOURCE_DIR}/deps/cmsis.lock" GUARD FILE TIMEOUT 600)

set(cmsis_core_dir "${PROJECT_SOURCE_DIR}/deps/cmsis-core-2b7495b8535bdcb306dac29b9ded4cfb679d7e5c")
set(cmsis_f0_dir "${PROJECT_SOURCE_DIR}/deps/cmsis-f0-f6fbbfcdc80797779a7a1eec87f1002516ef0483")
if(EXISTS "${cmsis_core_dir}/CMSIS/Core/Include/core_cm0.h")
    set(FETCHCONTENT_SOURCE_DIR_CORETX_CMSIS_CORE "${cmsis_core_dir}")
endif()
if(EXISTS "${cmsis_f0_dir}/Include/stm32f072xb.h")
    set(FETCHCONTENT_SOURCE_DIR_CORETX_CMSIS_F0 "${cmsis_f0_dir}")
endif()

# CMSIS-Core 5.9.0 supports native register mocks without Arm-only ACLE headers.
# Both dependencies are pinned to release commits and verified archives.
FetchContent_Declare(coretx_cmsis_core
    SOURCE_DIR "${cmsis_core_dir}"
    URL https://codeload.github.com/ARM-software/CMSIS_5/tar.gz/2b7495b8535bdcb306dac29b9ded4cfb679d7e5c
    URL_HASH SHA256=9f7f0080795b1635b82d9f7b084d37011866cd78365749997391ba0f9602f9fe
    SOURCE_SUBDIR CMSIS/Core/Include
)
FetchContent_Declare(coretx_cmsis_f0
    SOURCE_DIR "${cmsis_f0_dir}"
    URL https://codeload.github.com/STMicroelectronics/cmsis-device-f0/tar.gz/f6fbbfcdc80797779a7a1eec87f1002516ef0483
    URL_HASH SHA256=cb8744632efce52c93b575af4dec46b135b47363c01be7ff325ae12e0b879dad
    SOURCE_SUBDIR Include
)
FetchContent_MakeAvailable(coretx_cmsis_core coretx_cmsis_f0)

add_library(coretx_stm32f072_cmsis INTERFACE)
target_include_directories(coretx_stm32f072_cmsis SYSTEM INTERFACE
    "${coretx_cmsis_core_SOURCE_DIR}/CMSIS/Core/Include"
    "${coretx_cmsis_f0_SOURCE_DIR}/Include"
)
