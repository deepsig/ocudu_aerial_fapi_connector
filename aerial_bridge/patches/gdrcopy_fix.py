#!/usr/bin/env python3
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Patch GDRcopy to work without gdrdrv kernel module.

Simple approach: make gdr_open failure non-fatal and skip GDR buffer
creation when handle is null. The gpinned_buffer struct is NOT modified.
"""
import sys

# Patch 1: cuphydriver gpudevice.cpp — non-fatal gdr_open
filepath1 = "/opt/nvidia/aerial/cuPHY-CP/cuphydriver/src/common/gpudevice.cpp"
with open(filepath1, 'r') as f:
    c = f.read()

old1 = '''        gdrc_h = gdr_open();
        if(gdrc_h == nullptr)
            PHYDRIVER_THROW_EXCEPTIONS(-1, "GDRcopy open failed");'''
new1 = '''        gdrc_h = gdr_open();
        if(gdrc_h == nullptr)
        {
            fprintf(stderr, "WARNING: GDRcopy unavailable. GPU-direct RDMA disabled.\\n");
            init_gdr = false;
        }'''

if old1 in c:
    c = c.replace(old1, new1)
    with open(filepath1, 'w') as f: f.write(c)
    print(f"PATCHED: {filepath1}")
elif "GDRcopy unavailable" in c:
    print(f"Already patched: {filepath1}")

# Patch 2: aerial-fh-driver gpudevice.cpp
filepath2 = "/opt/nvidia/aerial/cuPHY-CP/aerial-fh-driver/app/fh_generator/src/gpudevice.cpp"
try:
    with open(filepath2, 'r') as f: c2 = f.read()
    old2 = '            THROW("GDRcopy open failed");'
    new2 = '            fprintf(stderr, "WARNING: GDRcopy unavailable in fh_generator\\n"); gdrc_h = nullptr;'
    if old2 in c2:
        c2 = c2.replace(old2, new2)
        with open(filepath2, 'w') as f: f.write(c2)
        print(f"PATCHED: {filepath2}")
except: pass

# Patch 3: Make newGDRbuf return a dummy buffer when GDR is disabled
# Instead of modifying gpinned_buffer, skip the allocation entirely
# and let the caller handle nullptr.
# BUT callers don't check nullptr... so we need a different approach.
#
# The actual fix: when init_gdr=false, don't create GpuDevice with
# init_gdr=true. The flag is set in the constructor config.
# Let's patch the caller to pass init_gdr=false.

# Actually the simplest: just don't call newGDRbuf at all when init_gdr=false.
# Patch phychannel.cpp to check before creating GDR buffer.
filepath3 = "/opt/nvidia/aerial/cuPHY-CP/cuphydriver/src/common/phychannel.cpp"
with open(filepath3, 'r') as f: c3 = f.read()

old3 = '    channel_complete_gdr.reset(gDev->newGDRbuf(1 * sizeof(uint32_t)));'
new3 = '''    if(gDev->getGDRhandler() && *gDev->getGDRhandler()) {
        channel_complete_gdr.reset(gDev->newGDRbuf(1 * sizeof(uint32_t)));
    } else {
        channel_complete_gdr.reset(); // GDRcopy disabled, skip GDR buffer
    }'''

if old3 in c3:
    c3 = c3.replace(old3, new3)

    # Also guard the dereference on the next line
    old3b = "    ((uint32_t*)channel_complete_gdr->addrh())[0] = 0;"
    new3b = "    if(channel_complete_gdr) ((uint32_t*)channel_complete_gdr->addrh())[0] = 0;"
    if old3b in c3:
        c3 = c3.replace(old3b, new3b)

    with open(filepath3, 'w') as f: f.write(c3)
    print(f"PATCHED: {filepath3} — skip GDR buffer when disabled")
elif "GDRcopy disabled" in c3:
    print(f"Already patched: {filepath3}")
else:
    print(f"WARNING: Pattern not found in {filepath3}")
