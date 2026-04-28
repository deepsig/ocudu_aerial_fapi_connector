#!/usr/bin/env python3
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Allow nvIPC SHM pools to run without CUDA host page locking on GB10.

On DGX Spark GB10, ``cudaHostRegister`` can fail for the SHM-backed CPU pools
used by Aerial's primary nvIPC transport. In the no-NIC local integration path
we do not need CUDA-mapped host SHM for correctness, so downgrade those
registration failures to warnings when ``AERIAL_SKIP_NIC_REG`` is enabled.
"""

from pathlib import Path
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"pattern not found for {label}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: nvipc_host_pinning_fix.py <nv_ipc_cuda_utils.cu>", file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    text = path.read_text()

    if "#include <stdlib.h>" not in text:
        text = replace_once(
            text,
            '#include <stdio.h>\n#include <string.h>\n#include <sys/mman.h>\n',
            '#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <sys/mman.h>\n',
            "stdlib include",
        )

    if "continuing without host page lock" not in text:
        text = replace_once(
            text,
            """    int flag = cudaHostRegisterPortable | cudaHostRegisterMapped;
    if(cudaHostRegister(phost, size, flag) != cudaSuccess)
    {
        checkLastCudaError();
        NVLOGE_NO_FMT(TAG, AERIAL_CUDA_API_EVENT, "{}: cudaHostRegister failed", __func__);
        return -1;
    }
    else
    {
        NVLOGI_FMT(TAG, "{}: OK", __func__);
        return 0;
    }""",
            """    int flag = cudaHostRegisterPortable | cudaHostRegisterMapped;
    cudaError_t err = cudaHostRegister(phost, size, flag);
    if(err != cudaSuccess)
    {
        checkLastCudaError();
        const char* skip_nic_reg = getenv("AERIAL_SKIP_NIC_REG");
        const bool tolerate_failure = (skip_nic_reg != NULL) && (strcmp(skip_nic_reg, "0") != 0);
        if(tolerate_failure && (err == cudaErrorInvalidValue || err == cudaErrorNotSupported))
        {
            fprintf(stderr, "WARNING: nvIPC cudaHostRegister failed; continuing without host page lock.\\n");
            return 0;
        }
        NVLOGE_NO_FMT(TAG, AERIAL_CUDA_API_EVENT, "{}: cudaHostRegister failed", __func__);
        return -1;
    }
    else
    {
        NVLOGI_FMT(TAG, "{}: OK", __func__);
        return 0;
    }""",
            "cudaHostRegister fallback",
        )

    if "host page lock was skipped earlier" not in text:
        text = replace_once(
            text,
            """    if(cudaHostUnregister(phost) != cudaSuccess)
    {
        checkLastCudaError();
        NVLOGE_NO_FMT(TAG, AERIAL_CUDA_API_EVENT, "{}: cudaHostUnregister failed", __func__);
        return -1;
    }
    else
    {
        NVLOGI_FMT(TAG, "{}: OK", __func__);
        return 0;
    }""",
            """    cudaError_t err = cudaHostUnregister(phost);
    if(err != cudaSuccess)
    {
        checkLastCudaError();
        const char* skip_nic_reg = getenv("AERIAL_SKIP_NIC_REG");
        const bool tolerate_failure = (skip_nic_reg != NULL) && (strcmp(skip_nic_reg, "0") != 0);
        if(tolerate_failure && (err == cudaErrorHostMemoryNotRegistered || err == cudaErrorInvalidValue))
        {
            fprintf(stderr, "WARNING: nvIPC cudaHostUnregister skipped because host page lock was skipped earlier.\\n");
            return 0;
        }
        NVLOGE_NO_FMT(TAG, AERIAL_CUDA_API_EVENT, "{}: cudaHostUnregister failed", __func__);
        return -1;
    }
    else
    {
        NVLOGI_FMT(TAG, "{}: OK", __func__);
        return 0;
    }""",
            "cudaHostUnregister fallback",
        )

    path.write_text(text)
    print(f"patched {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
