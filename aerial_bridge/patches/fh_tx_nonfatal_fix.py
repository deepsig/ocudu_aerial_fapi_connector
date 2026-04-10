#!/usr/bin/env python3
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Patch fronthaul operations to be non-fatal when no peers are registered.

On GB10 without GPUDirect RDMA, NIC registration fails and no fronthaul
peers exist. This patch adds peer_map.empty() guards to all FhProxy methods
that access peers, making them return early instead of crashing.
"""
import sys, re

# Patch fh.cpp — add guards to key methods
filepath = "/opt/nvidia/aerial/cuPHY-CP/cuphydriver/src/common/fh.cpp"
with open(filepath, 'r') as f:
    content = f.read()

# Guard prepareUPlanePackets
old = '''int FhProxy::prepareUPlanePackets(
    ru_type ru,
    peer_id_t peer_id, cudaStream_t dl_stream, t_ns start_tx_time,
    const slot_command_api::oran_slot_ind &slot_indication,
    const slot_command_api::slot_info_t &slot_info,
    struct umsg_fh_tx_msg& umsg_tx_list,
    size_t size, mod_compression_params* mod_comp_prm, mod_compression_params* mod_comp_config_temp, void * cb_obj, t_ns symbol_duration,
    cuphyBatchedMemcpyHelper& batchedMemcpyHelper)
{'''

new = '''int FhProxy::prepareUPlanePackets(
    ru_type ru,
    peer_id_t peer_id, cudaStream_t dl_stream, t_ns start_tx_time,
    const slot_command_api::oran_slot_ind &slot_indication,
    const slot_command_api::slot_info_t &slot_info,
    struct umsg_fh_tx_msg& umsg_tx_list,
    size_t size, mod_compression_params* mod_comp_prm, mod_compression_params* mod_comp_config_temp, void * cb_obj, t_ns symbol_duration,
    cuphyBatchedMemcpyHelper& batchedMemcpyHelper)
{
    if(peer_map.empty()) { return 0; } // No peers — FAPI-only mode'''

if old in content:
    content = content.replace(old, new)
    print("Guarded prepareUPlanePackets")
elif 'peer_map.empty()' in content:
    print("prepareUPlanePackets already guarded")

# Guard sendCPlane
old_sc = 'int FhProxy::sendCPlane('
idx = content.find(old_sc)
if idx >= 0 and 'peer_map.empty' not in content[idx:idx+500]:
    brace = content.find('{', idx)
    if brace > 0:
        content = content[:brace+1] + '\n    if(peer_map.empty()) { return 0; } // No peers — FAPI-only mode' + content[brace+1:]
        print("Guarded sendCPlane")

# Guard UserPlaneSendPackets
for method in ['UserPlaneSendPackets', 'UserPlaneSendPacketsGpuComm']:
    pattern = f'FhProxy::{method}('
    idx = content.find(pattern)
    if idx >= 0:
        brace = content.find('{', idx)
        if brace > 0 and 'peer_map.empty' not in content[idx:brace+200]:
            content = content[:brace+1] + '\n    if(peer_map.empty()) { return 0; } // No peers — FAPI-only mode' + content[brace+1:]
            print(f"Guarded {method}")

# Guard setTriggerTsGpuComm and triggerCqeTracerCb
for method in ['setTriggerTsGpuComm', 'triggerCqeTracerCb']:
    pattern = f'FhProxy::{method}('
    idx = content.find(pattern)
    if idx >= 0:
        brace = content.find('{', idx)
        if brace > 0 and 'peer_map.empty' not in content[idx:brace+200]:
            content = content[:brace+1] + '\n    if(peer_map.empty()) { return; } // No peers — FAPI-only mode' + content[brace+1:]
            print(f"Guarded {method}")

# Guard print_max_delays
pattern = 'FhProxy::print_max_delays('
idx = content.find(pattern)
if idx >= 0:
    brace = content.find('{', idx)
    if brace > 0 and 'peer_map.empty' not in content[idx:brace+200]:
        content = content[:brace+1] + '\n    if(peer_map.empty()) { return; } // No peers — FAPI-only mode' + content[brace+1:]
        print("Guarded print_max_delays")

# Guard getRxOrderItemsPeer
pattern = 'FhProxy::getRxOrderItemsPeer('
idx = content.find(pattern)
if idx >= 0:
    brace = content.find('{', idx)
    if brace > 0 and 'peer_map.empty' not in content[idx:brace+200]:
        content = content[:brace+1] + '\n    if(peer_map.empty()) { return nullptr; } // No peers — FAPI-only mode' + content[brace+1:]
        print("Guarded getRxOrderItemsPeer")

with open(filepath, 'w') as f:
    f.write(content)

print(f"Done patching {filepath}")
