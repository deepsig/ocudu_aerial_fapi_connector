# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

cmake_minimum_required(VERSION 3.22)
project(aerial_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Paths to OCUDU and Aerial installations ──────────────────

set(OCUDU_ROOT "/opt/ocudu" CACHE PATH "Path to OCUDU source tree")
set(AERIAL_ROOT "/opt/nvidia/aerial" CACHE PATH "Path to Aerial source tree")
set(AERIAL_BUILD_ROOT "/opt/nvidia/aerial/build" CACHE PATH "Path to Aerial build tree")

# ── OCUDU headers ────────────────────────────────────────────

set(OCUDU_INCLUDE_DIRS
    ${OCUDU_ROOT}/include
    ${OCUDU_ROOT}/external
    ${OCUDU_ROOT}/external/fmt/include
    ${OCUDU_ROOT}/external/CLI/include
    ${OCUDU_ROOT}/apps/units/flexible_o_du/split_6/o_du_high
    ${OCUDU_ROOT}/apps/units/flexible_o_du
    ${OCUDU_ROOT}/apps/units
    ${OCUDU_ROOT}/apps
)

# ── Aerial headers ───────────────────────────────────────────

set(AERIAL_INCLUDE_DIRS
    ${AERIAL_ROOT}/cuPHY-CP/gt_common_libs/nvIPC/include
    ${AERIAL_ROOT}/cuPHY-CP/scfl2adapter/lib/scf_5g_fapi
    ${AERIAL_ROOT}/cuPHY-CP/cuphyl2adapter/lib/nvPHY
)

# ── Aerial libraries ────────────────────────────────────────

find_library(NVIPC_LIB nvipc
    PATHS ${AERIAL_BUILD_ROOT}/cuPHY-CP/gt_common_libs/nvIPC
    NO_DEFAULT_PATH)

if(NOT NVIPC_LIB)
    message(WARNING "libnvipc.so not found at ${AERIAL_BUILD_ROOT}/cuPHY-CP/gt_common_libs/nvIPC — will try system path")
    find_library(NVIPC_LIB nvipc)
endif()

# ── Bridge library ───────────────────────────────────────────

add_library(aerial_bridge STATIC
    src/nvipc_transport.cpp
    src/rx_thread.cpp
    src/aerial_fapi_adaptor.cpp
    src/aerial_split6_plugin.cpp
    src/msg_translator.cpp
)

target_include_directories(aerial_bridge PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(aerial_bridge PRIVATE
    ${OCUDU_INCLUDE_DIRS}
    ${AERIAL_INCLUDE_DIRS}
)

# The deployed Aerial L1 in this repo expects the non-SCF_FAPI_10_04 wire layout
# (notably 16-bit TLV lengths in CONFIG/TX_DATA handling).

# Match OCUDU Release build flags.
target_compile_definitions(aerial_bridge PRIVATE NDEBUG)
target_compile_options(aerial_bridge PRIVATE -O3 -fPIC -fno-rtti)

# When this library provides the plugin factory, define this.
target_compile_definitions(aerial_bridge PRIVATE AERIAL_BRIDGE_REGISTER_PLUGIN)

if(NVIPC_LIB)
    target_link_libraries(aerial_bridge PRIVATE ${NVIPC_LIB})
    message(STATUS "Found libnvipc: ${NVIPC_LIB}")
else()
    message(WARNING "libnvipc not found — bridge will not link without it")
endif()

target_link_libraries(aerial_bridge PRIVATE pthread)

# ── GDR stub library (LD_PRELOAD for systems without gdrdrv) ─

add_library(gdr_stub SHARED src/gdr_stub.cpp)
set_target_properties(gdr_stub PROPERTIES
    OUTPUT_NAME "gdrapi"
    VERSION "2.5"
    SOVERSION "2")
target_compile_options(gdr_stub PRIVATE -fPIC)
# No external deps — uses plain mmap for memory allocation

# ── Test binary ──────────────────────────────────────────────

add_executable(bridge_test src/bridge_test.cpp)
target_include_directories(bridge_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AERIAL_INCLUDE_DIRS}
)
target_link_libraries(bridge_test PRIVATE aerial_bridge)
if(NVIPC_LIB)
    target_link_libraries(bridge_test PRIVATE ${NVIPC_LIB})
endif()
target_link_libraries(bridge_test PRIVATE pthread)

# ── Install ──────────────────────────────────────────────────

install(TARGETS aerial_bridge
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/aerial_bridge
    DESTINATION include
)
