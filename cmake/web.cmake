# SPDX-License-Identifier: GPL-2.0-or-later
include("${PROJECT_SOURCE_DIR}/cmake/cmsis.cmake")

set(CORETX_BOARD "I6X" CACHE STRING "Virtual transmitter: I6X or I6S")
if(NOT CORETX_BOARD STREQUAL "I6X" AND NOT CORETX_BOARD STREQUAL "I6S")
    message(FATAL_ERROR "CORETX_BOARD must be I6X or I6S")
endif()
if(CORETX_BOARD STREQUAL "I6S")
    set(web_profile i6s)
else()
    set(web_profile i6x)
endif()
add_executable(coretx_web platform/web/simulator.cpp)
target_link_libraries(coretx_web PRIVATE coretx coretx_stm32f072_cmsis)
set_target_properties(coretx_web PROPERTIES
    OUTPUT_NAME "coretx-${web_profile}" SUFFIX ".js" CXX_SCAN_FOR_MODULES OFF)
target_link_options(coretx_web PRIVATE
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sEXPORT_NAME=createCoreTX
    -sENVIRONMENT=web,node
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=1048576
    "-sEXPORTED_FUNCTIONS=['_sim_init','_sim_step','_sim_set_analog','_sim_set_switch','_sim_key','_sim_trim','_sim_touch','_sim_eeprom','_sim_revision','_sim_busy','_sim_status','_sim_lcd','_sim_channels','_sim_error','_sim_output']"
    "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','HEAP16','UTF8ToString']"
)
foreach(asset index.html simulator.js simulator.css)
    configure_file("${PROJECT_SOURCE_DIR}/platform/web/${asset}"
                   "${CMAKE_CURRENT_BINARY_DIR}/${asset}" COPYONLY)
endforeach()
