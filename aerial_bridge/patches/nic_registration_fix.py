#!/usr/bin/env python3
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Patch NIC registration to be non-fatal on GPUs without GPUDirect RDMA.

On GB10 (DGX Spark), DOCA cannot register GPU memory with the NIC because
GPUDirect RDMA is not supported (no dedicated HBM). This patch makes NIC
registration failure non-fatal, allowing cuphycontroller to continue without
the O-RAN fronthaul. L1 still functions via nvIPC for FAPI communication.
"""
import sys

filepath = sys.argv[1] if len(sys.argv) > 1 else "/opt/nvidia/aerial/cuPHY-CP/cuphydriver/src/common/context.cpp"

with open(filepath, 'r') as f:
    content = f.read()

# Both PhyDriverCtx constructors have this pattern
old_nic = '''        if (fh_proxy->registerNic(nic_cfg, ctx_cfg.gpu_id))
            PHYDRIVER_THROW_EXCEPTIONS(EINVAL, "NIC registration error");'''

new_nic = '''        if (fh_proxy->registerNic(nic_cfg, ctx_cfg.gpu_id))
        {
            fprintf(stderr, "WARNING: NIC registration failed (GPUDirect RDMA not available). Continuing without fronthaul.\\n");
            break; // Skip remaining NICs — fronthaul disabled
        }'''

count = content.count(old_nic)
if count > 0:
    content = content.replace(old_nic, new_nic)
    print(f"Patched {count} NIC registration site(s) to be non-fatal")
elif 'Continuing without fronthaul' in content:
    print("NIC registration already patched")
else:
    print("WARNING: Could not find NIC registration pattern")

with open(filepath, 'w') as f:
    f.write(content)

print(f"Done patching {filepath}")
