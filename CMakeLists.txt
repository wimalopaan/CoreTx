cmake_minimum_required(VERSION 3.30)
project(CoreTX LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(CORETX_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
option(CORETX_SIMULATOR "Build the SDL3 desktop simulator" OFF)
option(CORETX_STM32F072 "Build the STM32F072xB bare-metal firmware targets" OFF)
option(CORETX_ADC_COHERENT_SNAPSHOTS "Use two DMA halves for coherent I6X ADC scans" OFF)
option(CORETX_A7105 "Enable internal A7105 / AFHDS 2A" ON)
option(CORETX_CRSF "Enable external CRSF module" ON)
option(CORETX_MULTISWITCH "Enable MultiSwitch dashboards and SET4 commands (requires CRSF)" ON)
option(CORETX_MODUL_TANKMIX "Enable the managed tank mixer module" ON)
option(CORETX_EXTENDED_INPUTS "Enable optional i6X/i6S analog and digital hardware inputs" ON)
option(CORETX_SECURE_UI "Require RF OFF for protected UI operations" OFF)
option(CORETX_I2C_FAST_MODE "Use 400 kHz Fast mode instead of 100 kHz for storage I2C" ON)
set(CORETX_CRSF_REMOTE_TIMEOUT_MS "1000" CACHE STRING
    "CRSF parameter read timeout per request for receivers/remote devices in ms (100..10000)")
set(CORETX_CRSF_REMOTE_RETRIES "9" CACHE STRING
    "CRSF parameter read repetitions for receivers/remote devices (0..50)")
set(CORETX_CRSF_PARAMETER_IDS "256" CACHE STRING
    "CRSF parameter IDs 0..n-1 held in the configuration tree tables (32..256)")
set(CORETX_CRSF_CACHE_BYTES "1280" CACHE STRING
    "CRSF parameter definition cache in bytes (512..4096)")
set(CORETX_I6S_TOUCH_THRESHOLD "22" CACHE STRING
    "i6S FT6236 touch threshold TH_GROUP, lower is more sensitive (10..60)")
function(coretx_check_range name minimum maximum)
    if(NOT "${${name}}" MATCHES "^[0-9]+$" OR ${name} LESS ${minimum} OR ${name} GREATER ${maximum})
        message(FATAL_ERROR "${name} must be an integer in ${minimum}..${maximum}")
    endif()
endfunction()
coretx_check_range(CORETX_CRSF_REMOTE_TIMEOUT_MS 100 10000)
coretx_check_range(CORETX_CRSF_REMOTE_RETRIES 0 50)
coretx_check_range(CORETX_CRSF_PARAMETER_IDS 32 256)
coretx_check_range(CORETX_CRSF_CACHE_BYTES 512 4096)
coretx_check_range(CORETX_I6S_TOUCH_THRESHOLD 10 60)
set(CORETX_EDGETX_SOURCE "" CACHE PATH "Optional EdgeTX checkout to verify against the snapshot")
set(TRANSLATION "EN" CACHE STRING "Menu language")
set_property(CACHE TRANSLATION PROPERTY STRINGS EN DE)
if(NOT "${TRANSLATION}" MATCHES "^(EN|DE)$")
    message(FATAL_ERROR "TRANSLATION must be EN or DE")
endif()
set(CORETX_I6S OFF)
if(CORETX_BOARD STREQUAL "I6S")
    set(CORETX_I6S ON)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:-Wall>
        $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
        $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
        $<$<COMPILE_LANGUAGE:CXX>:-Wconversion>
        $<$<COMPILE_LANGUAGE:CXX>:-Wsign-conversion>
        $<$<COMPILE_LANGUAGE:CXX>:-Werror>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    )
    if(CORETX_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
elseif(CORETX_SANITIZERS)
    message(FATAL_ERROR "Sanitizers require GCC or Clang")
endif()

add_library(coretx INTERFACE)
target_include_directories(coretx INTERFACE "${PROJECT_SOURCE_DIR}")
target_compile_features(coretx INTERFACE cxx_std_26)
target_compile_definitions(coretx INTERFACE
    CORETX_A7105=$<BOOL:${CORETX_A7105}>
    CORETX_CRSF=$<BOOL:${CORETX_CRSF}>
    CORETX_MULTISWITCH=$<AND:$<BOOL:${CORETX_CRSF}>,$<BOOL:${CORETX_MULTISWITCH}>>
    CORETX_MODUL_TANKMIX=$<OR:$<BOOL:${CORETX_MODUL_TANKMIX}>,$<BOOL:$<TARGET_PROPERTY:CORETX_CONVERTER>>>
    CORETX_I6S=$<BOOL:${CORETX_I6S}>
    CORETX_EXTENDED_INPUTS=$<BOOL:${CORETX_EXTENDED_INPUTS}>
    CORETX_SECURE_UI=$<BOOL:${CORETX_SECURE_UI}>
    CORETX_I2C_FAST_MODE=$<BOOL:${CORETX_I2C_FAST_MODE}>
    CORETX_CRSF_REMOTE_TIMEOUT_MS=${CORETX_CRSF_REMOTE_TIMEOUT_MS}
    CORETX_CRSF_REMOTE_RETRIES=${CORETX_CRSF_REMOTE_RETRIES}
    CORETX_CRSF_PARAMETER_IDS=${CORETX_CRSF_PARAMETER_IDS}
    CORETX_CRSF_CACHE_BYTES=${CORETX_CRSF_CACHE_BYTES}
    CORETX_I6S_TOUCH_THRESHOLD=${CORETX_I6S_TOUCH_THRESHOLD}
)
if(NOT CORETX_STM32F072 AND TRANSLATION STREQUAL "DE")
    target_compile_definitions(coretx INTERFACE CORETX_LANGUAGE_DE)
endif()

if(CORETX_STM32F072)
    set(CORETX_BOARD "I6X" CACHE STRING "Transmitter board for the STM32F072 firmware")
    set_property(CACHE CORETX_BOARD PROPERTY STRINGS I6X I6S)
    if(NOT CORETX_BOARD STREQUAL "I6X" AND NOT CORETX_BOARD STREQUAL "I6S")
        message(FATAL_ERROR "CORETX_BOARD must be I6X or I6S")
    endif()
    if(NOT CMAKE_CROSSCOMPILING OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
       OR NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
        message(FATAL_ERROR "STM32F072 requires the ARM GCC toolchain and a separate build directory")
    endif()
    if(CORETX_SIMULATOR OR CORETX_SANITIZERS)
        message(FATAL_ERROR "The STM32F072 firmware does not support SDL or host sanitizers")
    endif()
    enable_language(ASM)
    include(cmake/cmsis.cmake)
    add_subdirectory(boards)
    return()
endif()

add_executable(coretx_demo examples/demo.cpp)
target_link_libraries(coretx_demo PRIVATE coretx)

add_executable(coretx_controls_demo examples/controls_demo.cpp)
target_link_libraries(coretx_controls_demo PRIVATE coretx)

add_library(coretx_ui INTERFACE)
target_link_libraries(coretx_ui INTERFACE coretx)

if(UNIX)
    add_library(coretx_file_storage platform/desktop/file_storage.cpp)
    target_link_libraries(coretx_file_storage PUBLIC coretx)
endif()

if(CORETX_SIMULATOR)
    if(NOT UNIX)
        message(FATAL_ERROR "Simulator file persistence currently requires a POSIX host")
    endif()
    find_package(SDL3 REQUIRED CONFIG)
    add_executable(coretx_simulator platform/desktop/simulator.cpp)
    target_link_libraries(coretx_simulator PRIVATE coretx_ui coretx_file_storage SDL3::SDL3)
endif()

include(CTest)
if(BUILD_TESTING)
    include(cmake/cmsis.cmake)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(coretx_fixed_bitset_tests tests/fixed_bitset_test.cpp)
    target_link_libraries(coretx_fixed_bitset_tests PRIVATE coretx)
    add_test(NAME coretx_fixed_bitset COMMAND coretx_fixed_bitset_tests)
    add_executable(coretx_fixed_vector_tests tests/fixed_vector_test.cpp)
    target_link_libraries(coretx_fixed_vector_tests PRIVATE coretx)
    add_test(NAME coretx_fixed_vector COMMAND coretx_fixed_vector_tests)
    add_executable(coretx_tests tests/core_test.cpp tests/edgetx_reference.cpp)
    target_link_libraries(coretx_tests PRIVATE coretx)
    add_test(NAME coretx COMMAND coretx_tests)

    add_executable(coretx_controls_tests tests/controls_test.cpp)
    target_link_libraries(coretx_controls_tests PRIVATE coretx)
    add_test(NAME coretx_controls COMMAND coretx_controls_tests)
    add_executable(coretx_input_setup_tests tests/input_setup_test.cpp)
    target_link_libraries(coretx_input_setup_tests PRIVATE coretx coretx_stm32f072_cmsis)
    add_test(NAME coretx_input_setup COMMAND coretx_input_setup_tests)

    add_executable(coretx_time_tests tests/time_test.cpp)
    target_link_libraries(coretx_time_tests PRIVATE coretx)
    add_test(NAME coretx_time COMMAND coretx_time_tests)

    add_executable(coretx_static_fixture_tests tests/static_fixture_test.cpp)
    target_link_libraries(coretx_static_fixture_tests PRIVATE coretx coretx_stm32f072_cmsis)
    add_test(NAME coretx_static_fixture COMMAND coretx_static_fixture_tests)

    add_executable(coretx_watchdog_tests tests/watchdog_test.cpp)
    target_link_libraries(coretx_watchdog_tests PRIVATE coretx_ui coretx_stm32f072_cmsis)
    add_test(NAME coretx_watchdog COMMAND coretx_watchdog_tests)
    add_executable(coretx_a7105_tests tests/a7105_test.cpp)
    target_link_libraries(coretx_a7105_tests PRIVATE coretx_ui coretx_stm32f072_cmsis)
    add_test(NAME coretx_a7105 COMMAND coretx_a7105_tests)
    add_executable(coretx_afhds2a_tests tests/afhds2a_test.cpp)
    target_link_libraries(coretx_afhds2a_tests PRIVATE coretx_ui coretx_stm32f072_cmsis)
    add_test(NAME coretx_afhds2a COMMAND coretx_afhds2a_tests)
    add_executable(coretx_telemetry_tests tests/telemetry_test.cpp)
    target_link_libraries(coretx_telemetry_tests PRIVATE coretx_ui)
    add_test(NAME coretx_telemetry COMMAND coretx_telemetry_tests)
    add_executable(coretx_multiswitch_tests tests/multiswitch_test.cpp)
    target_link_libraries(coretx_multiswitch_tests PRIVATE coretx_ui)
    add_test(NAME coretx_multiswitch COMMAND coretx_multiswitch_tests)
    add_executable(coretx_i6x_menu_tests tests/i6x_menu_test.cpp)
    target_link_libraries(coretx_i6x_menu_tests PRIVATE coretx_ui coretx_stm32f072_cmsis)
    add_test(NAME coretx_i6x_menu COMMAND coretx_i6x_menu_tests)

    add_executable(coretx_storage_tests tests/storage_test.cpp)
    target_link_libraries(coretx_storage_tests PRIVATE coretx)
    # CRC-checked storage fixtures exceed Clang's default constexpr evaluation budget.
    target_compile_options(coretx_storage_tests PRIVATE
        $<$<CXX_COMPILER_ID:Clang>:-fconstexpr-steps=4000000>)
    add_test(NAME coretx_storage COMMAND coretx_storage_tests)
    add_executable(coretx_model_converter_tests tests/model_converter_test.cpp)
    target_link_libraries(coretx_model_converter_tests PRIVATE coretx coretx_stm32f072_cmsis)
    set_property(TARGET coretx_model_converter_tests PROPERTY CORETX_CONVERTER TRUE)
    add_test(NAME coretx_model_converter COMMAND coretx_model_converter_tests)
    if(UNIX)
        add_executable(coretx_file_storage_tests tests/file_storage_test.cpp)
        target_link_libraries(coretx_file_storage_tests PRIVATE coretx_file_storage coretx_ui)
        add_test(NAME coretx_file_storage COMMAND coretx_file_storage_tests)
    endif()

    add_executable(coretx_ui_tests tests/ui_test.cpp)
    target_link_libraries(coretx_ui_tests PRIVATE coretx_ui)
    add_test(NAME coretx_ui COMMAND coretx_ui_tests)
    add_executable(coretx_calibration_ui_tests tests/calibration_ui_test.cpp)
    target_link_libraries(coretx_calibration_ui_tests PRIVATE coretx_ui)
    add_test(NAME coretx_calibration_ui COMMAND coretx_calibration_ui_tests)
    if(CORETX_SIMULATOR)
        add_test(NAME coretx_sdl_smoke COMMAND coretx_simulator --smoke-test)
        add_test(NAME coretx_sdl_touch_smoke COMMAND coretx_simulator --touch-smoke-test)
        add_test(NAME coretx_sdl_calibration_smoke COMMAND coretx_simulator --calibration-smoke-test)
        set_tests_properties(coretx_sdl_smoke coretx_sdl_touch_smoke coretx_sdl_calibration_smoke
            PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy" TIMEOUT 10)
        add_test(NAME coretx_sdl_storage COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/simulator_storage_test.py"
            "$<TARGET_FILE:coretx_simulator>")
        set_tests_properties(coretx_sdl_storage PROPERTIES TIMEOUT 60)
    endif()

    add_executable(coretx_pipeline_tests tests/pipeline_test.cpp tests/edgetx_pipeline.cpp)
    target_link_libraries(coretx_pipeline_tests PRIVATE coretx)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # Captured EdgeTX expressions retain their original implicit conversions.
        set_source_files_properties(tests/edgetx_pipeline.cpp PROPERTIES COMPILE_OPTIONS
            "-Wno-conversion;-Wno-sign-conversion;-Wno-unused-parameter")
    endif()
    add_test(NAME coretx_pipeline COMMAND coretx_pipeline_tests)
    add_executable(coretx_firmware_hardware_tests tests/firmware_hardware_test.cpp)
    target_link_libraries(coretx_firmware_hardware_tests PRIVATE coretx coretx_stm32f072_cmsis)
    target_compile_features(coretx_firmware_hardware_tests PRIVATE cxx_std_26)
    add_test(NAME coretx_firmware_hardware COMMAND coretx_firmware_hardware_tests)
    add_executable(coretx_i6x_diagnostic_tests tests/i6x_diagnostic_test.cpp)
    target_link_libraries(coretx_i6x_diagnostic_tests PRIVATE coretx_ui coretx_stm32f072_cmsis)
    add_test(NAME coretx_i6x_diagnostic COMMAND coretx_i6x_diagnostic_tests)
    add_executable(coretx_i6x_crsf_tests tests/i6x_crsf_test.cpp)
    target_link_libraries(coretx_i6x_crsf_tests PRIVATE coretx_ui coretx_stm32f072_cmsis)
    add_test(NAME coretx_i6x_crsf COMMAND coretx_i6x_crsf_tests)
    add_executable(coretx_crsf_config_tests tests/crsf_config_test.cpp)
    target_link_libraries(coretx_crsf_config_tests PRIVATE coretx_ui)
    add_test(NAME coretx_crsf_config COMMAND coretx_crsf_config_tests)
    add_executable(coretx_i6s_touch_tests tests/i6s_touch_test.cpp)
    target_link_libraries(coretx_i6s_touch_tests PRIVATE coretx_ui coretx_stm32f072_cmsis)
    add_test(NAME coretx_i6s_touch COMMAND coretx_i6s_touch_tests)
    add_executable(coretx_application_tests tests/application_test.cpp)
    target_link_libraries(coretx_application_tests PRIVATE coretx_ui coretx_stm32f072_cmsis)
    add_test(NAME coretx_application COMMAND coretx_application_tests)
    add_test(NAME edgetx_snapshot
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/sync_edgetx_reference.py" --check)
    add_test(NAME edgetx_capture
        COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/sync_reference_test.py")
    add_test(NAME coretx_clean
        COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/clean_test.py"
            "${CMAKE_COMMAND}")
    if(CORETX_EDGETX_SOURCE)
        add_test(NAME edgetx_source
            COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/sync_edgetx_reference.py"
                --check --source "${CORETX_EDGETX_SOURCE}")
    endif()
endif()
