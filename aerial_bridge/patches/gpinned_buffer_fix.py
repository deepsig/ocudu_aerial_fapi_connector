#!/usr/bin/env python3
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Patch gpinned_buffer to fall back to cudaHostAlloc when GDR is unavailable.

On GPUs without GPUDirect RDMA (e.g., GB10 Blackwell desktop where
nvidia_p2p_get_pages returns EINVAL), gpinned_buffer uses CUDA mapped
pinned memory instead of GDR-pinned memory.

Two fallback triggers:
1. GDR handle is null (gdr_open failed) — caught at constructor entry
2. gdr_pin_buffer fails (gdrdrv loaded but P2P not supported) — caught at pin

The addrh()/addrd() interface is identical — CPU accesses via host pointer,
GPU accesses via device pointer from cudaHostGetDevicePointer().
"""
import sys

filepath = sys.argv[1] if len(sys.argv) > 1 else "/opt/nvidia/aerial/cuPHY-CP/cuphydriver/include/gpudevice.hpp"

with open(filepath, 'r') as f:
    content = f.read()

# 1. Add useGDR member to protected section
old_members = '''    size_t     size_input;                                 ///< Requested buffer size in bytes
} gpinned_buffer;'''

new_members = '''    size_t     size_input;                                 ///< Requested buffer size in bytes
    bool       useGDR = true;                              ///< true=GDR path, false=cudaHostAlloc fallback
} gpinned_buffer;'''

if old_members in content:
    content = content.replace(old_members, new_members)
    print("Added useGDR member")
elif 'useGDR' in content:
    print("useGDR member already present")

# 2. Add fallback path at the beginning of constructor (after null/size check)
#    AND wrap gdr_pin_buffer failure to fall back instead of throwing

old_null_check = '''        if(g == nullptr || size_input == 0)
            PHYDRIVER_THROW_EXCEPTIONS(EINVAL, "gpinned_buffer bad input arguments");'''

new_null_check = '''        if(size_input == 0)
            PHYDRIVER_THROW_EXCEPTIONS(EINVAL, "gpinned_buffer: size cannot be 0");

        // Check if GDR is available
        if(g == nullptr || *g == nullptr)
        {
            // GDR unavailable — use CUDA mapped pinned memory as fallback.
            // This provides both host and device pointers without GPUDirect RDMA.
            useGDR = false;
            if(size_input < GPU_MIN_PIN_SIZE)
                size_input = GPU_MIN_PIN_SIZE;
            size_t rounded_size = (size_input + GPU_PAGE_SIZE - 1) & GPU_PAGE_MASK;
            size_free = rounded_size;

            void* h_ptr = nullptr;
            cudaError_t cerr = cudaHostAlloc(&h_ptr, rounded_size,
                                              cudaHostAllocMapped | cudaHostAllocPortable);
            if(cerr != cudaSuccess || h_ptr == nullptr)
                PHYDRIVER_THROW_EXCEPTIONS(EINVAL, "gpinned_buffer: cudaHostAlloc fallback failed");

            void* d_ptr = nullptr;
            cerr = cudaHostGetDevicePointer(&d_ptr, h_ptr, 0);
            if(cerr != cudaSuccess || d_ptr == nullptr)
            {
                cudaFreeHost(h_ptr);
                PHYDRIVER_THROW_EXCEPTIONS(EINVAL, "gpinned_buffer: cudaHostGetDevicePointer failed");
            }

            memset(h_ptr, 0, rounded_size);
            addr_h = (uintptr_t)h_ptr;
            addr_d = (uintptr_t)d_ptr;
            addr_free = (uintptr_t)h_ptr;
            static bool warned = false;
            if(!warned) { fprintf(stderr, "INFO: gpinned_buffer using CUDA mapped memory (no GDR)\\n"); warned = true; }
            return;
        }'''

if old_null_check in content:
    content = content.replace(old_null_check, new_null_check)
    print("Added cudaHostAlloc fallback path in constructor (null GDR handle)")
elif 'cudaHostAlloc fallback' in content:
    print("Null-handle fallback path already present")
else:
    # Try the version that was previously patched
    old_null_check2 = '''        if(size_input == 0)
            PHYDRIVER_THROW_EXCEPTIONS(EINVAL, "gpinned_buffer: size cannot be 0");

        // Check if GDR is available
        if(g == nullptr || *g == nullptr)'''
    if old_null_check2 in content:
        print("Null-handle fallback already present from previous patch")
    else:
        print("WARNING: Could not find null check pattern in constructor")

# 3. Replace gdr_pin_buffer failure: fall back to cudaHostAlloc instead of throwing
old_pin_fail = '''        if(0 != gdr_pin_buffer(*g, dev_addr, pin_size, 0, 0, &mh))
        {
            CU_CHECK_PHYDRIVER(cuMemFree(dev_addr));
            PHYDRIVER_THROW_EXCEPTIONS(EINVAL, "gdr_pin_buffer");
        }'''

# When gdr_pin_buffer fails (P2P not supported), free the cuMemAlloc'd buffer
# and fall back to cudaHostAlloc path.
new_pin_fail = '''        if(0 != gdr_pin_buffer(*g, dev_addr, pin_size, 0, 0, &mh))
        {
            // gdr_pin_buffer failed (P2P not supported on this GPU).
            // Fall back to CUDA mapped pinned memory.
            cuMemFree((CUdeviceptr)addr_free);
            useGDR = false;

            void* h_ptr = nullptr;
            cudaError_t cerr = cudaHostAlloc(&h_ptr, rounded_size,
                                              cudaHostAllocMapped | cudaHostAllocPortable);
            if(cerr != cudaSuccess || h_ptr == nullptr)
                PHYDRIVER_THROW_EXCEPTIONS(EINVAL, "gpinned_buffer: cudaHostAlloc fallback after pin fail");

            void* d_ptr = nullptr;
            cerr = cudaHostGetDevicePointer(&d_ptr, h_ptr, 0);
            if(cerr != cudaSuccess || d_ptr == nullptr)
            {
                cudaFreeHost(h_ptr);
                PHYDRIVER_THROW_EXCEPTIONS(EINVAL, "gpinned_buffer: cudaHostGetDevicePointer after pin fail");
            }

            memset(h_ptr, 0, rounded_size);
            addr_h = (uintptr_t)h_ptr;
            addr_d = (uintptr_t)d_ptr;
            addr_free = (uintptr_t)h_ptr;
            size_free = rounded_size;
            static bool pin_warned = false;
            if(!pin_warned) { fprintf(stderr, "INFO: gpinned_buffer gdr_pin_buffer failed, using CUDA mapped memory\\n"); pin_warned = true; }
            return;
        }'''

if old_pin_fail in content:
    content = content.replace(old_pin_fail, new_pin_fail)
    print("Patched gdr_pin_buffer failure to fall back to cudaHostAlloc")
elif 'gdr_pin_buffer failed' in content:
    print("gdr_pin_buffer fallback already present")
else:
    print("WARNING: Could not find gdr_pin_buffer failure pattern")

# 4. Patch destructor
old_destructor = '''        gdr_unmap(*g, mh, (void*)addr_h, size_free);
        gdr_unpin_buffer(*g, mh);
        CU_CHECK_PHYDRIVER(cuMemFree((CUdeviceptr)addr_free));'''

new_destructor = '''        if(useGDR) {
            gdr_unmap(*g, mh, (void*)addr_h, size_free);
            gdr_unpin_buffer(*g, mh);
            CU_CHECK_PHYDRIVER(cuMemFree((CUdeviceptr)addr_free));
        } else {
            cudaFreeHost((void*)addr_free);
        }'''

# Try alternate destructor patterns
old_destructor2 = '''        if(h_ptr)
        {
            gdr_unmap(*g, mh, h_ptr, size);
            gdr_unpin_buffer(*g, mh);
        }
        cudaFree((void*)d_ptr);'''

if old_destructor in content:
    content = content.replace(old_destructor, new_destructor)
    print("Patched destructor (pattern 1)")
elif old_destructor2 in content:
    content = content.replace(old_destructor2, new_destructor)
    print("Patched destructor (pattern 2)")
elif 'useGDR' in content and 'cudaFreeHost' in content:
    print("Destructor already patched")
else:
    print("WARNING: Could not find destructor pattern")

with open(filepath, 'w') as f:
    f.write(content)

print(f"Done patching {filepath}")
