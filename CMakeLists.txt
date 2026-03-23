# Jettyd Firmware SDK — Top-level CMakeLists.txt
#
# This file is used when building a specific device configuration.
# The build tool (tools/build.py) sets JETTYD_DEVICE to select the target.

cmake_minimum_required(VERSION 3.16)

# Device selection — set by build.py or manually
if(NOT DEFINED JETTYD_DEVICE)
    message(FATAL_ERROR "JETTYD_DEVICE not set. Use: idf.py build -DJETTYD_DEVICE=soil-moisture-v1")
endif()

set(JETTYD_DEVICE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/devices/${JETTYD_DEVICE}")
if(NOT EXISTS "${JETTYD_DEVICE_DIR}")
    message(FATAL_ERROR "Device directory not found: ${JETTYD_DEVICE_DIR}")
endif()

# Extra component directories — core + selected drivers
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/drivers"
)

# Device-specific main.c and generated files
set(MAIN_SRCS
    "${JETTYD_DEVICE_DIR}/main.c"
    "${JETTYD_DEVICE_DIR}/build/driver_init.c"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(jettyd_firmware)
