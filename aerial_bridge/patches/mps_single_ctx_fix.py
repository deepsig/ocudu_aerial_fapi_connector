#!/usr/bin/env python3
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Patch mps.cpp to use a single shared CUDA context on GPUs without MPS SM affinity.

On GPUs like GB10 (Blackwell desktop), cuCtxCreate with SM affinity returns
CUDA_ERROR_NOT_SUPPORTED. Without this patch, each MPS instance would get a
separate full-GPU context, causing "invalid resource handle" errors when
streams/events created in one context are used in another.

Fix: All MPS instances share ONE CUcontext when the affinity call fails.
"""
import sys

filepath = sys.argv[1] if len(sys.argv) > 1 else "/opt/nvidia/aerial/cuPHY-CP/cuphydriver/src/common/mps.cpp"

with open(filepath, 'r') as f:
    content = f.read()

# Step 1: Add static shared context variables AFTER the includes (where CUcontext is defined)
# Look for the include block
old_includes = '''#include "mps.hpp"
#include "context.hpp"
#include "nvlog.hpp"'''

new_includes = '''#include "mps.hpp"
#include "context.hpp"
#include "nvlog.hpp"

// Shared fallback context for GPUs without MPS SM affinity (e.g., GB10 Blackwell desktop).
// All MpsCtx instances share this single context to avoid cross-context stream/event issues.
static CUcontext s_sharedFallbackCtx = nullptr;
static int s_sharedFallbackRefCount = 0;'''

if old_includes in content and 's_sharedFallbackCtx' not in content:
    content = content.replace(old_includes, new_includes)
    print("Added shared fallback context variables (after includes)")
elif 's_sharedFallbackCtx' in content:
    # Already has the variables — check if they're in the wrong place (before includes)
    tag_line = '#define TAG (NVLOG_TAG_BASE_CUPHY_DRIVER + 27)'
    idx_tag = content.find(tag_line)
    idx_static = content.find('static CUcontext s_sharedFallbackCtx')
    idx_include = content.find('#include "mps.hpp"')
    if idx_static < idx_include:
        # Variables are before includes — move them after
        # Remove the old static lines
        lines = content.split('\n')
        new_lines = []
        skip_next_blank = False
        for line in lines:
            if 's_sharedFallbackCtx' in line or 's_sharedFallbackRefCount' in line:
                skip_next_blank = True
                continue
            if skip_next_blank and line.strip() == '':
                skip_next_blank = False
                continue
            if '// Shared fallback context' in line or '// All MpsCtx instances share' in line:
                continue
            skip_next_blank = False
            new_lines.append(line)
        content = '\n'.join(new_lines)
        # Now add them after includes
        content = content.replace(old_includes, new_includes)
        print("Moved shared context variables after includes")
    else:
        print("Shared context variables already present in correct location")

# Step 2: Wrap the cuCtxCreate call in a try-catch for CUDA >= 13000
# The actual code uses CU_CHECK_PHYDRIVER which throws on error.
old_ctx_create_13 = '''#if CUDA_VERSION >= 13000
            CUctxCreateParams ctxParams{};
            ctxParams.execAffinityParams = &affinityPrm;
            ctxParams.numExecAffinityParams = 1;
            ctxParams.cigParams = nullptr;
            CU_CHECK_PHYDRIVER(cuCtxCreate(&cuCtx, &ctxParams, CU_CTX_SCHED_SPIN | CU_CTX_MAP_HOST, cuDev));
#else
            CU_CHECK_PHYDRIVER(cuCtxCreate_v3(&cuCtx, &affinityPrm, 1, CU_CTX_SCHED_SPIN | CU_CTX_MAP_HOST, cuDev));
#endif'''

new_ctx_create_13 = '''#if CUDA_VERSION >= 13000
            {
                CUctxCreateParams ctxParams{};
                ctxParams.execAffinityParams = &affinityPrm;
                ctxParams.numExecAffinityParams = 1;
                ctxParams.cigParams = nullptr;
                CUresult mpsResult = cuCtxCreate(&cuCtx, &ctxParams, CU_CTX_SCHED_SPIN | CU_CTX_MAP_HOST, cuDev);
                if (mpsResult != CUDA_SUCCESS) {
                    // SM affinity not supported — use shared fallback context
                    if (s_sharedFallbackCtx == nullptr) {
                        fprintf(stderr, "WARNING: MPS SM affinity not supported (err=%d). Creating single shared CUDA context.\\n", (int)mpsResult);
                        // Use PRIMARY context — ensures runtime API (cudaStreamCreate etc.)
                        // uses the same context as driver API. cuCtxCreate_v2 creates a
                        // non-primary context that causes "invalid resource handle" when
                        // runtime API streams are used with driver API context.
                        CUresult stdResult = cuDevicePrimaryCtxRetain(&s_sharedFallbackCtx, cuDev);
                        if (stdResult != CUDA_SUCCESS) {
                            throw cuphy::cuda_driver_exception(stdResult, "cuDevicePrimaryCtxRetain shared fallback failed");
                        }
                        cuCtxSetCurrent(s_sharedFallbackCtx);
                    }
                    cuCtx = s_sharedFallbackCtx;
                    s_sharedFallbackRefCount++;
                    devSmCount = actualDevSmCount;
                }
            }
#else
            {
                CUresult mpsResult = cuCtxCreate_v3(&cuCtx, &affinityPrm, 1, CU_CTX_SCHED_SPIN | CU_CTX_MAP_HOST, cuDev);
                if (mpsResult != CUDA_SUCCESS) {
                    if (s_sharedFallbackCtx == nullptr) {
                        fprintf(stderr, "WARNING: MPS SM affinity not supported (err=%d). Creating single shared CUDA context.\\n", (int)mpsResult);
                        // Use PRIMARY context — ensures runtime API (cudaStreamCreate etc.)
                        // uses the same context as driver API. cuCtxCreate_v2 creates a
                        // non-primary context that causes "invalid resource handle" when
                        // runtime API streams are used with driver API context.
                        CUresult stdResult = cuDevicePrimaryCtxRetain(&s_sharedFallbackCtx, cuDev);
                        if (stdResult != CUDA_SUCCESS) {
                            throw cuphy::cuda_driver_exception(stdResult, "cuDevicePrimaryCtxRetain shared fallback failed");
                        }
                        cuCtxSetCurrent(s_sharedFallbackCtx);
                    }
                    cuCtx = s_sharedFallbackCtx;
                    s_sharedFallbackRefCount++;
                    devSmCount = actualDevSmCount;
                }
            }
#endif'''

if old_ctx_create_13 in content:
    content = content.replace(old_ctx_create_13, new_ctx_create_13)
    print("Patched cuCtxCreate with shared fallback (CUDA 13000+ and v3 paths)")
elif 's_sharedFallbackCtx' in content and 'mpsResult' in content:
    print("Context creation fallback already patched")
else:
    print("WARNING: Could not find cuCtxCreate pattern — trying alternate")
    # Try matching just the CUDA >= 13000 block
    if 'CU_CHECK_PHYDRIVER(cuCtxCreate(&cuCtx, &ctxParams' in content:
        content = content.replace(
            'CU_CHECK_PHYDRIVER(cuCtxCreate(&cuCtx, &ctxParams, CU_CTX_SCHED_SPIN | CU_CTX_MAP_HOST, cuDev));',
            '''CUresult mpsResult = cuCtxCreate(&cuCtx, &ctxParams, CU_CTX_SCHED_SPIN | CU_CTX_MAP_HOST, cuDev);
                if (mpsResult != CUDA_SUCCESS) {
                    if (s_sharedFallbackCtx == nullptr) {
                        fprintf(stderr, "WARNING: MPS not supported (err=%d). Shared fallback context.\\n", (int)mpsResult);
                        CUresult stdResult = cuDevicePrimaryCtxRetain(&s_sharedFallbackCtx, cuDev);
                        if (stdResult != CUDA_SUCCESS) throw cuphy::cuda_driver_exception(stdResult, "cuDevicePrimaryCtxRetain fallback");
                        cuCtxSetCurrent(s_sharedFallbackCtx);
                    }
                    cuCtx = s_sharedFallbackCtx;
                    s_sharedFallbackRefCount++;
                    devSmCount = actualDevSmCount;
                }''')
        print("Patched cuCtxCreate inline (CUDA 13000 path only)")
    if 'CU_CHECK_PHYDRIVER(cuCtxCreate_v3(&cuCtx' in content:
        content = content.replace(
            'CU_CHECK_PHYDRIVER(cuCtxCreate_v3(&cuCtx, &affinityPrm, 1, CU_CTX_SCHED_SPIN | CU_CTX_MAP_HOST, cuDev));',
            '''CUresult mpsResult = cuCtxCreate_v3(&cuCtx, &affinityPrm, 1, CU_CTX_SCHED_SPIN | CU_CTX_MAP_HOST, cuDev);
                if (mpsResult != CUDA_SUCCESS) {
                    if (s_sharedFallbackCtx == nullptr) {
                        fprintf(stderr, "WARNING: MPS not supported (err=%d). Shared fallback context.\\n", (int)mpsResult);
                        CUresult stdResult = cuDevicePrimaryCtxRetain(&s_sharedFallbackCtx, cuDev);
                        if (stdResult != CUDA_SUCCESS) throw cuphy::cuda_driver_exception(stdResult, "cuDevicePrimaryCtxRetain fallback");
                        cuCtxSetCurrent(s_sharedFallbackCtx);
                    }
                    cuCtx = s_sharedFallbackCtx;
                    s_sharedFallbackRefCount++;
                    devSmCount = actualDevSmCount;
                }''')
        print("Patched cuCtxCreate_v3 inline")

# Step 3: Patch destructor for shared context
old_destructor = '''    if(!isGreenContext && (devSmCount > 0))
    {
        CU_CHECK_PHYDRIVER(cuCtxSynchronize());
        CU_CHECK_PHYDRIVER(cuCtxDestroy(cuCtx));
        ctxDestroyed = true;
    }'''

new_destructor = '''    if(!isGreenContext && (devSmCount > 0))
    {
        if (cuCtx == s_sharedFallbackCtx) {
            // Shared primary context — release when last reference dropped
            s_sharedFallbackRefCount--;
            if (s_sharedFallbackRefCount <= 0) {
                CU_CHECK_PHYDRIVER(cuCtxSynchronize());
                cuDevicePrimaryCtxRelease(cuDev);
                s_sharedFallbackCtx = nullptr;
            }
        } else {
            CU_CHECK_PHYDRIVER(cuCtxSynchronize());
            CU_CHECK_PHYDRIVER(cuCtxDestroy(cuCtx));
        }
        ctxDestroyed = true;
    }'''

if old_destructor in content:
    content = content.replace(old_destructor, new_destructor)
    print("Patched destructor for shared context")
elif 's_sharedFallbackCtx' in content and 'sharedFallbackRefCount--' in content:
    print("Destructor already patched")
else:
    print("WARNING: Could not find destructor pattern")

with open(filepath, 'w') as f:
    f.write(content)

print(f"Done patching {filepath}")
