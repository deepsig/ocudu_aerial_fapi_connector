#!/usr/bin/env bash
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# Helper script to retry Aerial build inside the container.
# Useful after installing additional dependencies (e.g. DOCA SDK).
set -euo pipefail

echo "=== Building Aerial ==="
cd /opt/nvidia/aerial
mkdir -p build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=cuPHY/cmake/toolchains/native
make -j$(nproc)
echo "=== Aerial build complete ==="
