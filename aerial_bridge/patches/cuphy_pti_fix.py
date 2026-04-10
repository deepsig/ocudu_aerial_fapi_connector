#!/usr/bin/env python3
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Patch cuphy_pti.cpp to handle BAR mmap failure gracefully.

When the NIC PCI BAR cannot be mmapped (kernel security restriction),
this patch makes PTI init return early without crash. PTI timing
functions will use d_ptpreg=nullptr which callers should handle.
"""
import sys

filepath = sys.argv[1] if len(sys.argv) > 1 else "/opt/nvidia/aerial/cuPHY/src/cuphy/cuphy_pti.cpp"

with open(filepath, 'r') as f:
    content = f.read()

# Strategy: when BAR mmap fails, skip all CUDA registration and set
# d_ptpreg = nullptr. Return early — PTI timing will be disabled.
old_block = '''    void* m = mmap(0, PCIE_BAR0_SIZE, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
    if (m == reinterpret_cast<void*>(-1))
    {
        char err_string[3000];
        snprintf(err_string,2998,"Unable to mmap PCIE_BAR0 using %s",filename);
        perror(err_string);
        exit(1);
    }'''

new_block = '''    void* m = mmap(0, PCIE_BAR0_SIZE, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
    if (m == reinterpret_cast<void*>(-1))
    {
        // BAR mmap failed — PTI hardware timing unavailable.
        // This is non-critical; the L1 will use software timing instead.
        fprintf(stderr, "WARNING: PTI disabled (NIC BAR mmap failed). Using software timing.\\n");
        close(fd);
        d_ptpreg = nullptr;
        // Allocate host-pinned timing buffers (still needed by other code)
        CHECK_CUDA(cudaMallocHost(&hd_ptp_time_ns, sizeof(uint64_t)*MAX_TIMING_KERNEL_RECORDS));
        CHECK_CUDA(cudaMallocHost(&hd_gpu_time_v0_ns, sizeof(uint64_t)*MAX_TIMING_KERNEL_RECORDS));
        CHECK_CUDA(cudaMallocHost(&hd_gpu_time_v1_ns, sizeof(uint64_t)*MAX_TIMING_KERNEL_RECORDS));
        memset(hd_ptp_time_ns, 0, sizeof(uint64_t)*MAX_TIMING_KERNEL_RECORDS);
        memset(hd_gpu_time_v0_ns, 0, sizeof(uint64_t)*MAX_TIMING_KERNEL_RECORDS);
        memset(hd_gpu_time_v1_ns, 0, sizeof(uint64_t)*MAX_TIMING_KERNEL_RECORDS);
        return;
    }'''

if old_block in content:
    content = content.replace(old_block, new_block)
    with open(filepath, 'w') as f:
        f.write(content)
    print("PATCHED: cuphy_pti.cpp — BAR mmap failure returns early with d_ptpreg=nullptr")
else:
    print("WARNING: Pattern not found — file may already be patched or has different formatting")
    # Try to check if already patched
    if "PTI disabled" in content:
        print("File appears already patched")
    else:
        print("ERROR: Could not patch file")
        sys.exit(1)
