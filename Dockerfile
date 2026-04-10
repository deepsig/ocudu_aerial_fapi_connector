# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ============================================================
# Combined build environment for NVIDIA Aerial + OCUDU
# Base: Official Aerial NGC container (includes MathDx, DOCA,
#       cuFFTdx, CLI11, fmt, WiseEnum, and all Aerial deps)
# ============================================================
FROM nvcr.io/nvidia/aerial/aerial-cuda-accelerated-ran:25-3-cubb

ARG DEBIAN_FRONTEND=noninteractive
ARG AERIAL_TAG=25.3.2
ARG OCUDU_TAG=release_26_04

LABEL maintainer="deepsig"
LABEL description="Combined build environment for NVIDIA Aerial and OCUDU"

# ============================================================
# Additional packages for OCUDU and Aerial source build
# (NGC base already has: CUDA 12.9, DOCA 3.0, MathDx, CLI11,
#  fmt, WiseEnum, gsl-lite, ZMQ, sctp, protobuf, gRPC, etc.)
# ============================================================
USER root
RUN mkdir -p /var/lib/apt/lists/partial && \
    apt-get update && apt-get install -y --no-install-recommends \
    # OCUDU build deps not in NGC base
    libfftw3-dev libmbedtls-dev libyaml-cpp-dev \
    # General build tools that may be missing
    ccache ninja-build pkg-config \
    git-lfs \
    # Utilities
    vim nano \
    && rm -rf /var/lib/apt/lists/*

# ============================================================
# Build GoogleTest from source (matches GCC ABI)
# ============================================================
RUN git clone --depth 1 --branch v1.14.0 \
        https://github.com/google/googletest.git /tmp/googletest && \
    cd /tmp/googletest && mkdir build && cd build && \
    cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON && \
    make -j$(nproc) && make install && ldconfig && \
    rm -rf /tmp/googletest

# ============================================================
# Clone and build NVIDIA Aerial from source
# ============================================================
WORKDIR /opt/nvidia
RUN git lfs install && \
    git clone --depth 1 --branch ${AERIAL_TAG} --recurse-submodules \
        https://github.com/NVIDIA/aerial-cuda-accelerated-ran.git aerial && \
    cd aerial && git lfs pull

ENV cuBB_SDK=/opt/nvidia/aerial

# Fix DPDK pkg-config: it has both -march and -mcpu which conflict
# with the Aerial toolchain flags. Keep only -mcpu=neoverse-n1.
RUN sed -i 's/-march=armv8.2-a+crypto -mcpu=neoverse-n1/-mcpu=neoverse-n1/' \
    /opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig/libdpdk-libs.pc

# Patch toolchain: replace -march=native with -mcpu=neoverse-n1
# to avoid conflict with DPDK's pkg-config flags
RUN sed -i 's/-march=native/-mcpu=neoverse-n1/' \
    /opt/nvidia/aerial/cuPHY/cmake/toolchains/native

WORKDIR /opt/nvidia/aerial
# Add SM_120 (Blackwell GB10) to CUDA architectures — default only has 80+90
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_TOOLCHAIN_FILE=cuPHY/cmake/toolchains/native -DBUILD_DOCS=OFF \
    -DCMAKE_CUDA_ARCHITECTURES="80-real;90-real;120" \
    2>&1 | tee /tmp/aerial-cmake.log || true && \
    make -j$(nproc) 2>&1 | tee /tmp/aerial-build.log || true

# ============================================================
# Clone and build OCUDU
# ============================================================
WORKDIR /opt
RUN git clone --depth 1 --branch ${OCUDU_TAG} \
        https://gitlab.com/ocudu/ocudu.git

COPY ocudu_patches/slot_point_extender_adaptor.h \
     /opt/ocudu/apps/units/flexible_o_du/split_6/o_du_high/slot_point_extender_adaptor.h
COPY ocudu_patches/slot_point_extender_adaptor.cpp \
     /opt/ocudu/apps/units/flexible_o_du/split_6/o_du_high/slot_point_extender_adaptor.cpp
COPY aerial_bridge/patches/ocudu_executor_queue_fix.py /tmp/ocudu_executor_queue_fix.py
RUN python3 /tmp/ocudu_executor_queue_fix.py /opt/ocudu/lib/du/du_high/du_high_executor_mapper.cpp

WORKDIR /opt/ocudu
RUN mkdir -p build && cd build && \
    cmake ../ \
        -DENABLE_EXPORT=ON \
        -DENABLE_ZEROMQ=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DMCPU=neoverse-v1+crypto && \
    make -j$(nproc)

# ============================================================
# Build Aerial FAPI Bridge Plugin
# ============================================================
COPY aerial_bridge /opt/aerial_bridge
WORKDIR /opt/aerial_bridge
RUN mkdir -p build && cd build && \
    cmake .. \
        -DOCUDU_ROOT=/opt/ocudu \
        -DAERIAL_ROOT=/opt/nvidia/aerial \
        -DAERIAL_BUILD_ROOT=/opt/nvidia/aerial/build && \
    make -j$(nproc)

# ============================================================
# Link bridge into gnb: replace dummy split6 plugin, add deps, re-link
# ============================================================
WORKDIR /opt/ocudu
# Replace dummy plugin source with our bridge plugin
RUN cp /opt/aerial_bridge/src/aerial_split6_plugin.cpp \
       apps/units/flexible_o_du/split_6/o_du_high/split6_plugin_dummy.cpp && \
    # Patch the CMakeLists to add bridge sources and link deps
    cd apps/units/flexible_o_du/split_6/o_du_high && \
    cat > CMakeLists.txt << 'CMAKE'
set(SOURCES
        slot_point_extender_adaptor.cpp
        split6_o_du_application_unit_impl.cpp
        split6_o_du_factory.cpp
        split6_o_du_impl.cpp
        split6_o_du_unit_cli11_schema.cpp
        split6_o_du_unit_config_validator.cpp)

# Aerial FAPI bridge plugin (replaces dummy)
add_library(ocudu_split6_plugin STATIC
    split6_plugin_dummy.cpp
    /opt/aerial_bridge/src/aerial_fapi_adaptor.cpp
    /opt/aerial_bridge/src/nvipc_transport.cpp
    /opt/aerial_bridge/src/rx_thread.cpp
    /opt/aerial_bridge/src/msg_translator.cpp)
target_include_directories(ocudu_split6_plugin PRIVATE
    /opt/aerial_bridge/include
    /opt/nvidia/aerial/cuPHY-CP/gt_common_libs/nvIPC/include
    /opt/nvidia/aerial/cuPHY-CP/scfl2adapter/lib/scf_5g_fapi
    /opt/nvidia/aerial/cuPHY-CP/cuphyl2adapter/lib/nvPHY
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/external
    ${CMAKE_SOURCE_DIR}/external/fmt/include
    ${CMAKE_SOURCE_DIR}/external/CLI/include
    ${CMAKE_SOURCE_DIR}/apps/units/flexible_o_du/split_6/o_du_high
    ${CMAKE_SOURCE_DIR}/apps/units/flexible_o_du
    ${CMAKE_SOURCE_DIR}/apps/units
    ${CMAKE_SOURCE_DIR}/apps)
target_compile_definitions(ocudu_split6_plugin PRIVATE
    NDEBUG AERIAL_BRIDGE_REGISTER_PLUGIN)
target_compile_options(ocudu_split6_plugin PRIVATE -O3 -fPIC -fno-rtti)
target_link_libraries(ocudu_split6_plugin PRIVATE
    /opt/nvidia/aerial/build/cuPHY-CP/gt_common_libs/nvIPC/libnvipc.so
    pthread)

add_library(ocudu_flexible_o_du_split_6 STATIC ${SOURCES})
target_include_directories(ocudu_flexible_o_du_split_6 PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ocudu_flexible_o_du_split_6
                                                    ocudu_o_du
                                                    ocudu_o_du_high_unit_helpers
                                                    ocudu_split6_plugin
                                                    ocudu_metrics_json_generators)
CMAKE
# Build split6 plugin and the split6 O-DU unit (excluded from default build)
RUN cd build && \
    make -j$(nproc) ocudu_split6_plugin 2>&1 | tail -5 && \
    make -j$(nproc) ocudu_flexible_o_du_split_6 2>&1 | tail -5

# Patch cuphy_pti.cpp: fallback to anonymous mmap when BAR mmap fails.
# PTI timing reads zeros instead of real NIC PTP timestamps — non-critical.
COPY aerial_bridge/patches/cuphy_pti_fix.py /tmp/cuphy_pti_fix.py
RUN python3 /tmp/cuphy_pti_fix.py /opt/nvidia/aerial/cuPHY/src/cuphy/cuphy_pti.cpp && \
    cd /opt/nvidia/aerial/build && \
    make -j$(nproc) cuphy 2>&1 | tail -3 && \
    make -j$(nproc) cuphycontroller_scf 2>&1 | tail -3

# Patch MPS: share ONE CUDA context when MPS SM affinity not supported (GB10).
# Without this, 8 separate contexts cause "invalid resource handle" errors.
COPY aerial_bridge/patches/mps_single_ctx_fix.py /tmp/mps_single_ctx_fix.py
RUN python3 /tmp/mps_single_ctx_fix.py /opt/nvidia/aerial/cuPHY-CP/cuphydriver/src/common/mps.cpp && \
    cd /opt/nvidia/aerial/build && \
    bash -o pipefail -c 'make -j$(nproc) cuphycontroller_scf 2>&1 | tail -5'

# Patch GDRcopy: make gdr_open non-fatal (sets init_gdr=false when gdrdrv unavailable)
ARG CACHE_BUST_GDR=3
COPY aerial_bridge/patches/gdrcopy_fix.py /tmp/gdrcopy_fix.py
RUN python3 /tmp/gdrcopy_fix.py && \
    cd /opt/nvidia/aerial/build && \
    cmake .. -DCMAKE_TOOLCHAIN_FILE=cuPHY/cmake/toolchains/native -DBUILD_DOCS=OFF \
    -DCMAKE_CUDA_ARCHITECTURES="80-real;90-real;120" > /dev/null 2>&1 && \
    bash -o pipefail -c 'make -j$(nproc) cuphy cuphy_ldpc cuphydriver cuphycontroller_scf 2>&1 | tail -10'

# Patch gpinned_buffer: cudaHostAlloc fallback when GDR handle is null (GB10 no P2P)
COPY aerial_bridge/patches/gpinned_buffer_fix.py /tmp/gpinned_buffer_fix.py
RUN python3 /tmp/gpinned_buffer_fix.py /opt/nvidia/aerial/cuPHY-CP/cuphydriver/include/gpudevice.hpp && \
    cd /opt/nvidia/aerial/build && \
    bash -o pipefail -c 'make -j$(nproc) cuphydriver cuphycontroller_scf l2_adapter_cuphycontroller_scf 2>&1 | tail -10'

# Patch NIC registration: non-fatal when GPUDirect RDMA unavailable (GB10)
COPY aerial_bridge/patches/nic_registration_fix.py /tmp/nic_registration_fix.py
RUN python3 /tmp/nic_registration_fix.py /opt/nvidia/aerial/cuPHY-CP/cuphydriver/src/common/context.cpp && \
    cd /opt/nvidia/aerial/build && \
    bash -o pipefail -c 'make -j$(nproc) cuphycontroller_scf 2>&1 | tail -5'

# Patch no-fronthaul mode: disable GPU comm/GDR paths and make FH NIC-empty safe.
COPY aerial_bridge/patches/gpu_comm_standalone_fix.py /tmp/gpu_comm_standalone_fix.py
RUN python3 /tmp/gpu_comm_standalone_fix.py /opt/nvidia/aerial/cuPHY-CP/cuphydriver/src/common/context.cpp && \
    cd /opt/nvidia/aerial/build && \
    bash -o pipefail -c 'make -j$(nproc) cuphydriver cuphycontroller_scf 2>&1 | tail -10'

# Patch L1 standalone mode: fix l1_staticBFWConfigured and l1_resetDBTStorage
# to return 0 instead of -1 in the standalone else-branch.
RUN FILE=/opt/nvidia/aerial/cuPHY-CP/cuphyl2adapter/lib/nvPHY/nv_phy_driver_proxy.cpp && \
    sed -i '/l1_staticBFWConfiguredInFH/,/return -1/{s/return -1;/return 0; \/\/ patched for standalone/}' $FILE && \
    sed -i '/l1_resetDBTStorageInFH/,/return -1/{s/return -1;/return 0; \/\/ patched for standalone/}' $FILE && \
    grep "patched for standalone" $FILE && \
    cd /opt/nvidia/aerial/build && \
    bash -o pipefail -c 'make -j$(nproc) l2_adapter_cuphycontroller_scf 2>&1 | tail -5'

# Patch gnb to link against split_6 instead of split_dynamic, then rebuild
RUN sed -i 's/ocudu_flexible_o_du_split_dynamic/ocudu_flexible_o_du_split_6/' \
        apps/gnb/CMakeLists.txt && \
    cd build && cmake .. > /dev/null 2>&1 && \
    bash -o pipefail -c 'make -j$(nproc) gnb 2>&1 | tail -10'

# ============================================================
# Runtime scripts and default working directory
# ============================================================
COPY scripts/start_l1.sh /usr/local/bin/start_l1.sh
COPY scripts/make_loopback_config.py /usr/local/bin/make_loopback_config.py
RUN chmod +x /usr/local/bin/start_l1.sh /usr/local/bin/make_loopback_config.py

WORKDIR /workspace
CMD ["/bin/bash"]
