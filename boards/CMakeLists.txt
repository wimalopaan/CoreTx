find_package(Python3 REQUIRED COMPONENTS Interpreter)

string(TOLOWER "${CORETX_BOARD}" board_name)
set(firmware_targets)
foreach(language IN ITEMS en de)
    set(firmware "coretx-${board_name}-${language}")
    add_executable(${firmware} common/main.cpp common/rf_interrupts.cpp
        "${PROJECT_SOURCE_DIR}/platform/stm32f072/startup.S")
    if(CORETX_BOARD STREQUAL "I6X")
        target_compile_definitions(${firmware} PRIVATE
            CORETX_ADC_COHERENT_SNAPSHOTS=$<BOOL:${CORETX_ADC_COHERENT_SNAPSHOTS}>)
    endif()
    if(language STREQUAL "de")
        target_compile_definitions(${firmware} PRIVATE CORETX_LANGUAGE_DE)
    endif()
    list(APPEND firmware_targets ${firmware})
endforeach()

if(CORETX_BOARD STREQUAL "I6S")
    add_executable(coretx-i6s-touch-trace EXCLUDE_FROM_ALL common/main.cpp common/rf_interrupts.cpp
        "${PROJECT_SOURCE_DIR}/platform/stm32f072/startup.S")
    target_compile_definitions(coretx-i6s-touch-trace PRIVATE CORETX_I6S_TOUCH_TRACE)
    list(APPEND firmware_targets coretx-i6s-touch-trace)
    add_executable(coretx-i6s-touch-diagnostic EXCLUDE_FROM_ALL i6s/touch_diagnostic_main.cpp
        "${PROJECT_SOURCE_DIR}/platform/stm32f072/startup.S")
    list(APPEND firmware_targets coretx-i6s-touch-diagnostic)
    add_executable(coretx-i6s-measurement EXCLUDE_FROM_ALL i6s/measurement_main.cpp
        "${PROJECT_SOURCE_DIR}/platform/stm32f072/startup.S")
    list(APPEND firmware_targets coretx-i6s-measurement)
endif()

foreach(firmware IN LISTS firmware_targets)
    set_target_properties(${firmware} PROPERTIES
        SUFFIX ".elf"
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
        CXX_SCAN_FOR_MODULES OFF
        LINK_DEPENDS "${PROJECT_SOURCE_DIR}/platform/stm32f072/stm32f072xb.ld;${PROJECT_SOURCE_DIR}/tools/report_firmware.py"
    )
    target_compile_options(${firmware} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-ffreestanding>
        $<$<COMPILE_LANGUAGE:CXX>:-ffunction-sections>
        $<$<COMPILE_LANGUAGE:CXX>:-fdata-sections>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-unwind-tables>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-asynchronous-unwind-tables>
        $<$<COMPILE_LANGUAGE:CXX>:-fstack-usage>
        $<$<COMPILE_LANGUAGE:CXX>:-g>
    )
    target_link_options(${firmware} PRIVATE
        -nostdlib
        "-T${PROJECT_SOURCE_DIR}/platform/stm32f072/stm32f072xb.ld"
        -Wl,--gc-sections
        -Wl,--build-id=none
        -Wl,-z,noexecstack
        -Wl,--fatal-warnings
        "-Wl,-Map=${PROJECT_BINARY_DIR}/${firmware}.map"
        -Wl,--print-memory-usage
    )
    target_link_libraries(${firmware} PRIVATE coretx coretx_stm32f072_cmsis
        -Wl,--start-group c gcc -Wl,--end-group)

    add_custom_command(TARGET ${firmware} POST_BUILD
        BYPRODUCTS "${PROJECT_BINARY_DIR}/${firmware}.bin"
            "${PROJECT_BINARY_DIR}/${firmware}.hex"
            "${PROJECT_BINARY_DIR}/${firmware}-resource-report.txt"
        COMMAND "${CMAKE_OBJCOPY}" -O binary
            "$<TARGET_FILE:${firmware}>" "${PROJECT_BINARY_DIR}/${firmware}.bin"
        COMMAND "${CMAKE_OBJCOPY}" -O ihex
            "$<TARGET_FILE:${firmware}>" "${PROJECT_BINARY_DIR}/${firmware}.hex"
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/report_firmware.py"
            --elf "$<TARGET_FILE:${firmware}>"
            --size "${CMAKE_SIZE}" --nm "${CMAKE_NM}"
            --stack-dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${firmware}.dir"
            --output "${PROJECT_BINARY_DIR}/${firmware}-resource-report.txt"
        VERBATIM
    )
endforeach()
