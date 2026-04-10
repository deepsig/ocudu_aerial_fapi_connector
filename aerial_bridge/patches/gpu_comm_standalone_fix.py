#!/usr/bin/env python3
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Disable fronthaul GPU comm and FH-dependent paths in standalone/FAPI-only mode.

On GB10 we run Aerial without usable GPUDirect RDMA / O-RAN fronthaul peers.
The existing non-fatal fronthaul guards prevent some crashes, but startup still
enables both fronthaul GPU comm tasks and unconditional GDRCopy allocation.
When NIC registration fails we also need the FH layer to degrade into a no-op
instead of touching empty peer/NIC state. Reaching the first real FAPI DL slot
also exposed a latent Aerial bug where DL task count accounting assumes every
optional aggregate is present, which pushes a null task pointer.

The remaining important distinction is that OCUDU FAPI integration runs in the
normal SCF path, not Aerial's internal ``standalone: 1`` simulator. In that
mode we can still have zero registered FH peers, and that also needs to disable
GPU-comm/compression sidecar work.
"""

from pathlib import Path
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"pattern not found for {label}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: gpu_comm_standalone_fix.py <context.cpp>", file=sys.stderr)
        return 2

    context_path = Path(sys.argv[1])
    if context_path.name != "context.cpp" or len(context_path.parents) < 2:
        print("usage: gpu_comm_standalone_fix.py <context.cpp>", file=sys.stderr)
        return 2
    fh_hpp_path = context_path.parents[2] / "include" / "fh.hpp"
    constant_hpp_path = context_path.parents[2] / "include" / "constant.hpp"
    fh_cpp_path = context_path.parent / "fh.cpp"
    api_cpp_path = context_path.parent / "cuphydriver_api.cpp"
    dl_task_cpp_path = context_path.parents[1] / "downlink" / "task_function_dl_aggr.cpp"
    slot_map_dl_cpp_path = context_path.parents[1] / "downlink" / "slot_map_dl.cpp"
    order_entity_cpp_path = context_path.parents[1] / "uplink" / "order_entity.cpp"
    slot_commands_cpp_path = context_path.parents[3] / "scfl2adapter" / "lib" / "scf_5g_fapi" / "scf_5g_slot_commands.cpp"
    scf_phy_cpp_path = context_path.parents[3] / "scfl2adapter" / "lib" / "scf_5g_fapi" / "scf_5g_fapi_phy.cpp"

    context_text = context_path.read_text()
    fh_hpp_text = fh_hpp_path.read_text()
    constant_hpp_text = constant_hpp_path.read_text()
    fh_cpp_text = fh_cpp_path.read_text()
    api_cpp_text = api_cpp_path.read_text()
    dl_task_cpp_text = dl_task_cpp_path.read_text()
    slot_map_dl_cpp_text = slot_map_dl_cpp_path.read_text()
    order_entity_cpp_text = order_entity_cpp_path.read_text()
    slot_commands_cpp_text = slot_commands_cpp_path.read_text()
    scf_phy_cpp_text = scf_phy_cpp_path.read_text()

    if "standalone mode disables fronthaul GPU comm paths" not in context_text:
        context_text = replace_once(
            context_text,
            """bool PhyDriverCtx::gpuCommDlEnabled(void) const {
    if(enable_gpu_comm_dl == 1)
        return true;
    return false;
}""",
            """bool PhyDriverCtx::gpuCommDlEnabled(void) const {
    // In standalone/FAPI-only mode there are no usable fronthaul GPU comm peers.
    if(standalone) {
        return false; // standalone mode disables fronthaul GPU comm paths
    }
    if(enable_gpu_comm_dl == 1)
        return true;
    return false;
}""",
            "gpuCommDlEnabled",
        )

        context_text = replace_once(
            context_text,
            """bool PhyDriverCtx::gpuCommEnabledViaCpu(void) const {
    if(enable_gpu_comm_via_cpu == 1)
        return true;
    return false;
}""",
            """bool PhyDriverCtx::gpuCommEnabledViaCpu(void) const {
    if(standalone) {
        return false; // standalone mode disables fronthaul GPU comm paths
    }
    if(enable_gpu_comm_via_cpu == 1)
        return true;
    return false;
}""",
            "gpuCommEnabledViaCpu",
        )

        context_text = replace_once(
            context_text,
            """bool PhyDriverCtx::cpuCommEnabled(void) const {
    if(enable_cpu_init_comms == 1)
        return true;
    return false;
}""",
            """bool PhyDriverCtx::cpuCommEnabled(void) const {
    if(standalone) {
        return false; // standalone mode disables fronthaul GPU comm paths
    }
    if(enable_cpu_init_comms == 1)
        return true;
    return false;
}""",
            "cpuCommEnabled",
        )

    if "registered fronthaul peers" not in context_text:
        context_text = replace_once(
            context_text,
            """bool PhyDriverCtx::gpuCommDlEnabled(void) const {
    // In standalone/FAPI-only mode there are no usable fronthaul GPU comm peers.
    if(standalone) {
        return false; // standalone mode disables fronthaul GPU comm paths
    }
    if(enable_gpu_comm_dl == 1)
        return true;
    return false;
}""",
            """bool PhyDriverCtx::gpuCommDlEnabled(void) const {
    // FAPI-only mode must also disable fronthaul GPU comm work when no peers are registered.
    if(standalone) {
        return false; // standalone mode disables fronthaul GPU comm paths
    }
    if(!fh_proxy || !fh_proxy->hasRegisteredPeers()) {
        return false; // no registered fronthaul peers
    }
    if(enable_gpu_comm_dl == 1)
        return true;
    return false;
}""",
            "gpuCommDlEnabled no-peers guard",
        )

        context_text = replace_once(
            context_text,
            """bool PhyDriverCtx::gpuCommEnabledViaCpu(void) const {
    if(standalone) {
        return false; // standalone mode disables fronthaul GPU comm paths
    }
    if(enable_gpu_comm_via_cpu == 1)
        return true;
    return false;
}""",
            """bool PhyDriverCtx::gpuCommEnabledViaCpu(void) const {
    if(standalone) {
        return false; // standalone mode disables fronthaul GPU comm paths
    }
    if(!fh_proxy || !fh_proxy->hasRegisteredPeers()) {
        return false; // no registered fronthaul peers
    }
    if(enable_gpu_comm_via_cpu == 1)
        return true;
    return false;
}""",
            "gpuCommEnabledViaCpu no-peers guard",
        )

        context_text = replace_once(
            context_text,
            """bool PhyDriverCtx::cpuCommEnabled(void) const {
    if(standalone) {
        return false; // standalone mode disables fronthaul GPU comm paths
    }
    if(enable_cpu_init_comms == 1)
        return true;
    return false;
}""",
            """bool PhyDriverCtx::cpuCommEnabled(void) const {
    if(standalone) {
        return false; // standalone mode disables fronthaul GPU comm paths
    }
    if(!fh_proxy || !fh_proxy->hasRegisteredPeers()) {
        return false; // no registered fronthaul peers
    }
    if(enable_cpu_init_comms == 1)
        return true;
    return false;
}""",
            "cpuCommEnabled no-peers guard",
        )

    if "ctx_cfg.gpu_id, !standalone" not in context_text:
        context_text = replace_once(
            context_text,
            """    std::unique_ptr<GpuDevice> g = std::unique_ptr<GpuDevice>(new GpuDevice((phydriver_handle)this, ctx_cfg.gpu_id, true));""",
            """    // Standalone/FAPI-only mode has no GPUDirect RDMA producer, so skip GDRCopy setup.
    std::unique_ptr<GpuDevice> g = std::unique_ptr<GpuDevice>(
        new GpuDevice((phydriver_handle)this, ctx_cfg.gpu_id, !standalone));""",
            "GpuDevice GDR init",
        )

    pdsch_pool_old = """static constexpr uint32_t PHY_PDSCH_AGGR_X_CTX = 10;                ///< PDSCH aggregation objects per context"""
    pdsch_pool_new = """static constexpr uint32_t PHY_PDSCH_AGGR_X_CTX = 24;                ///< PDSCH aggregation objects per context"""
    if pdsch_pool_new not in constant_hpp_text and pdsch_pool_old in constant_hpp_text:
        constant_hpp_text = replace_once(
            constant_hpp_text,
            pdsch_pool_old,
            pdsch_pool_new,
            "PDSCH aggregate pool depth",
        )

    pdcch_dl_pool_old = """static constexpr uint32_t PHY_PDCCH_DL_AGGR_X_CTX = 10;             ///< DL PDCCH aggregation objects per context"""
    pdcch_dl_pool_new = """static constexpr uint32_t PHY_PDCCH_DL_AGGR_X_CTX = 24;             ///< DL PDCCH aggregation objects per context"""
    if pdcch_dl_pool_new not in constant_hpp_text and pdcch_dl_pool_old in constant_hpp_text:
        constant_hpp_text = replace_once(
            constant_hpp_text,
            pdcch_dl_pool_old,
            pdcch_dl_pool_new,
            "PDCCH DL aggregate pool depth",
        )

    nic_failure_guard = """        if (fh_proxy->registerNic(nic_cfg, ctx_cfg.gpu_id))
        {
            fprintf(stderr, "WARNING: NIC registration failed (GPUDirect RDMA not available). Continuing without fronthaul.\\n");
            break; // Skip remaining NICs
        }"""
    nic_failure_guard_replacement = """        if (fh_proxy->registerNic(nic_cfg, ctx_cfg.gpu_id))
        {
            fprintf(stderr, "WARNING: NIC registration failed (GPUDirect RDMA not available). Continuing without fronthaul.\\n");
            enable_gpu_comm_dl = 0;
            enable_gpu_comm_via_cpu = 0;
            enable_cpu_init_comms = 0;
            break; // Skip remaining NICs — fronthaul disabled
        }"""
    while nic_failure_guard in context_text:
        context_text = replace_once(
            context_text,
            nic_failure_guard,
            nic_failure_guard_replacement,
            "registerNic failure downgrade",
        )

    nic_loop = """    for (auto nic_cfg : ctx_cfg.nic_configs) {
        if (fh_proxy->registerNic(nic_cfg, ctx_cfg.gpu_id))
        {
            fprintf(stderr, "WARNING: NIC registration failed (GPUDirect RDMA not available). Continuing without fronthaul.\\n");
            enable_gpu_comm_dl = 0;
            enable_gpu_comm_via_cpu = 0;
            enable_cpu_init_comms = 0;
            break; // Skip remaining NICs — fronthaul disabled
        }
    }"""
    nic_loop_replacement = """    const bool skip_nic_reg = (std::getenv("AERIAL_SKIP_NIC_REG") != nullptr) &&
                              (std::strcmp(std::getenv("AERIAL_SKIP_NIC_REG"), "0") != 0);
    if(skip_nic_reg)
    {
        fprintf(stderr, "INFO: skipping FH NIC registration (AERIAL_SKIP_NIC_REG enabled).\\n");
        enable_gpu_comm_dl = 0;
        enable_gpu_comm_via_cpu = 0;
        enable_cpu_init_comms = 0;
    }
    else
    {
        for (auto nic_cfg : ctx_cfg.nic_configs) {
            if (fh_proxy->registerNic(nic_cfg, ctx_cfg.gpu_id))
            {
                fprintf(stderr, "WARNING: NIC registration failed (GPUDirect RDMA not available). Continuing without fronthaul.\\n");
                enable_gpu_comm_dl = 0;
                enable_gpu_comm_via_cpu = 0;
                enable_cpu_init_comms = 0;
                break; // Skip remaining NICs — fronthaul disabled
            }
        }
    }"""
    while nic_loop in context_text:
        context_text = replace_once(
            context_text,
            nic_loop,
            nic_loop_replacement,
            "skip NIC registration env guard",
        )

    if "nic_map.empty() ? aerial_fh::BfwCplaneChainingMode::NO_CHAINING : bfw_c_plane_chaining_mode" not in fh_hpp_text:
        fh_hpp_text = replace_once(
            fh_hpp_text,
            """    [[nodiscard]] aerial_fh::BfwCplaneChainingMode getBfwCPlaneChainingMode() const { return bfw_c_plane_chaining_mode; } ///< Get beamforming weight C-plane chaining mode""",
            """    [[nodiscard]] aerial_fh::BfwCplaneChainingMode getBfwCPlaneChainingMode() const { return nic_map.empty() ? aerial_fh::BfwCplaneChainingMode::NO_CHAINING : bfw_c_plane_chaining_mode; } ///< Get beamforming weight C-plane chaining mode""",
            "getBfwCPlaneChainingMode no-op",
        )

    if "hasRegisteredPeers() const" not in fh_hpp_text:
        fh_hpp_text = replace_once(
            fh_hpp_text,
            """    aerial_fh::NicHandle getNic(std::string &nic_bus_addr) {
        return nic_map[nic_bus_addr]; 
    }
""",
            """    aerial_fh::NicHandle getNic(std::string &nic_bus_addr) {
        return nic_map[nic_bus_addr]; 
    }

    [[nodiscard]] bool hasRegisteredPeers() const { return !peer_map.empty(); }
""",
            "hasRegisteredPeers helper",
        )

    if "if(nic_map.empty())" not in fh_cpp_text:
        fh_cpp_text = replace_once(
            fh_cpp_text,
            """int FhProxy::registerMem(MemRegInfo const* memreg_info, MemRegHandle* memreg)
{
    PhyDriverCtx* pdctx = StaticConversion<PhyDriverCtx>(pdh).get();
    int           ret   = 0;
""",
            """int FhProxy::registerMem(MemRegInfo const* memreg_info, MemRegHandle* memreg)
{
    if(nic_map.empty())
    {
        return 0;
    }

    PhyDriverCtx* pdctx = StaticConversion<PhyDriverCtx>(pdh).get();
    int           ret   = 0;
""",
            "registerMem no-op without NICs",
        )

    dl_task_count_replacement = """            dl_task_count = 0; // Recompute from the instantiated DL work for this slot.
            const bool has_dl_control_work = (aggr_pdcch_dl_ptr || aggr_pdcch_ul_ptr || aggr_pbch_ptr || aggr_csirs_ptr);
            const bool has_gpu_comm_prepare = pdctx->gpuCommDlEnabled();
            const bool has_fh_peers = pdctx->getFhProxy() && pdctx->getFhProxy()->hasRegisteredPeers();

            if(!has_fh_peers)
            {
                num_dlc_tasks = 0;
                if(dlbfw_only) //Test for only DLBFW presence
                {
                    // Account for DLBFW + DL Task 3 cleanup.
                    dl_task_count += 2;
                }
                else
                {
                    // FAPI-only mode: keep cuPHY work and cleanup, skip FH callback/C-plane/compression tasks.
                    dl_task_count += 1; // DL Task 3 cleanup
                    dl_task_count += aggr_pdsch_ptr ? 1 : 0;
                    dl_task_count += has_dl_control_work ? 1 : 0;
                    dl_task_count += aggr_dlbfw_ptr ? 1 : 0;
                }
                dl_worker_offset=1;
            }
            else if(pdctx->gpuCommEnabledViaCpu()){
                if(dlbfw_only) //Test for only DLBFW presence
                {
                    // Account for DLBFW + DL Task 3 cleanup.
                    dl_task_count += 2;
                }
                else
                {
                    // Count only the DL tasks that will actually be instantiated for this slot.
                    dl_task_count += 4 + num_dlc_tasks; // FHCB + DL task 2 + compression + cleanup + C-plane fanout.
                    dl_task_count += aggr_pdsch_ptr ? 1 : 0;
                    dl_task_count += has_dl_control_work ? 1 : 0;
                    dl_task_count += aggr_dlbfw_ptr ? 1 : 0;
                    dl_task_count += has_gpu_comm_prepare ? num_dlc_tasks : 0;
                    dl_task_count += 1; // CPU doorbell task.
                }
                dl_worker_offset=2;
            }
            else
            {
                if(dlbfw_only) //Test for only DLBFW presence
                {
                    // Account for DLBFW + DL Task 3 cleanup.
                    dl_task_count += 2;
                }
                else
                {
                    // Count only the DL tasks that will actually be instantiated for this slot.
                    dl_task_count += 4 + num_dlc_tasks; // FHCB + DL task 2 + compression + cleanup + C-plane fanout.
                    dl_task_count += aggr_pdsch_ptr ? 1 : 0;
                    dl_task_count += has_dl_control_work ? 1 : 0;
                    dl_task_count += aggr_dlbfw_ptr ? 1 : 0;
                    dl_task_count += has_gpu_comm_prepare ? num_dlc_tasks : 0;
                }
                dl_worker_offset=1;
            }"""

    original_dl_count_block = """            if(pdctx->gpuCommEnabledViaCpu()){
                if(dlbfw_only) //Test for only DLBFW presence
                {
                    //Account for only DLBFW + DL Task 3 Buf clean up tasks
                    dl_task_count+=1;    
                }
                else
                {
                    dl_task_count+= 5 + (num_dlc_tasks<<1); //Add Compression Task + DL Task 2 (Tx) + DL Task 3 + C-Plane Tasks (x2 to factor in UPlane prepare) + FHCB + CPU Door bell task
                }                
                dl_worker_offset=2;
            }
            else
            {
                if(dlbfw_only) //Test for only DLBFW presence
                {
                    //Account for only DLBFW + DL Task 3 Buf clean up tasks
                    dl_task_count+=1;    
                }
                else
                {
                    dl_task_count+= 4 + (num_dlc_tasks<<1); //Add Compression Task + DL Task 2 (Tx) + DL Task 3 + C-Plane Tasks (x2 to factor in UPlane prepare) + FHCB
                }                
                dl_worker_offset=1;
            }"""
    partial_dl_count_block = """            const bool has_dl_control_work = (aggr_pdcch_dl_ptr || aggr_pdcch_ul_ptr || aggr_pbch_ptr || aggr_csirs_ptr);
            const bool has_gpu_comm_prepare = pdctx->gpuCommDlEnabled();

            if(pdctx->gpuCommEnabledViaCpu()){
                if(dlbfw_only) //Test for only DLBFW presence
                {
                    // Account for DLBFW + DL Task 3 cleanup.
                    dl_task_count += 1;
                }
                else
                {
                    // Count only the DL tasks that will actually be instantiated for this slot.
                    dl_task_count += 4 + num_dlc_tasks; // FHCB + DL task 2 + compression + cleanup + C-plane fanout.
                    dl_task_count += aggr_pdsch_ptr ? 1 : 0;
                    dl_task_count += has_dl_control_work ? 1 : 0;
                    dl_task_count += has_gpu_comm_prepare ? num_dlc_tasks : 0;
                    dl_task_count += 1; // CPU doorbell task.
                }
                dl_worker_offset=2;
            }
            else
            {
                if(dlbfw_only) //Test for only DLBFW presence
                {
                    // Account for DLBFW + DL Task 3 cleanup.
                    dl_task_count += 1;
                }
                else
                {
                    // Count only the DL tasks that will actually be instantiated for this slot.
                    dl_task_count += 4 + num_dlc_tasks; // FHCB + DL task 2 + compression + cleanup + C-plane fanout.
                    dl_task_count += aggr_pdsch_ptr ? 1 : 0;
                    dl_task_count += has_dl_control_work ? 1 : 0;
                    dl_task_count += has_gpu_comm_prepare ? num_dlc_tasks : 0;
                }
                dl_worker_offset=1;
            }"""

    current_dl_count_block = """            dl_task_count = 0; // Recompute from the instantiated DL work for this slot.
            const bool has_dl_control_work = (aggr_pdcch_dl_ptr || aggr_pdcch_ul_ptr || aggr_pbch_ptr || aggr_csirs_ptr);
            const bool has_gpu_comm_prepare = pdctx->gpuCommDlEnabled();

            if(pdctx->gpuCommEnabledViaCpu()){
                if(dlbfw_only) //Test for only DLBFW presence
                {
                    // Account for DLBFW + DL Task 3 cleanup.
                    dl_task_count += 2;
                }
                else
                {
                    // Count only the DL tasks that will actually be instantiated for this slot.
                    dl_task_count += 4 + num_dlc_tasks; // FHCB + DL task 2 + compression + cleanup + C-plane fanout.
                    dl_task_count += aggr_pdsch_ptr ? 1 : 0;
                    dl_task_count += has_dl_control_work ? 1 : 0;
                    dl_task_count += aggr_dlbfw_ptr ? 1 : 0;
                    dl_task_count += has_gpu_comm_prepare ? num_dlc_tasks : 0;
                    dl_task_count += 1; // CPU doorbell task.
                }
                dl_worker_offset=2;
            }
            else
            {
                if(dlbfw_only) //Test for only DLBFW presence
                {
                    // Account for DLBFW + DL Task 3 cleanup.
                    dl_task_count += 2;
                }
                else
                {
                    // Count only the DL tasks that will actually be instantiated for this slot.
                    dl_task_count += 4 + num_dlc_tasks; // FHCB + DL task 2 + compression + cleanup + C-plane fanout.
                    dl_task_count += aggr_pdsch_ptr ? 1 : 0;
                    dl_task_count += has_dl_control_work ? 1 : 0;
                    dl_task_count += aggr_dlbfw_ptr ? 1 : 0;
                    dl_task_count += has_gpu_comm_prepare ? num_dlc_tasks : 0;
                }
                dl_worker_offset=1;
            }"""

    if "const bool has_fh_peers = pdctx->getFhProxy() && pdctx->getFhProxy()->hasRegisteredPeers();" not in api_cpp_text and original_dl_count_block in api_cpp_text:
        api_cpp_text = replace_once(
            api_cpp_text,
            original_dl_count_block,
            dl_task_count_replacement,
            "dl_task_count dynamic accounting",
        )

    if "const bool has_fh_peers = pdctx->getFhProxy() && pdctx->getFhProxy()->hasRegisteredPeers();" not in api_cpp_text and partial_dl_count_block in api_cpp_text:
        api_cpp_text = replace_once(
            api_cpp_text,
            partial_dl_count_block,
            dl_task_count_replacement,
            "dl_task_count partial accounting upgrade",
        )

    if "const bool has_fh_peers = pdctx->getFhProxy() && pdctx->getFhProxy()->hasRegisteredPeers();" not in api_cpp_text and current_dl_count_block in api_cpp_text:
        api_cpp_text = replace_once(
            api_cpp_text,
            current_dl_count_block,
            dl_task_count_replacement,
            "dl_task_count current accounting upgrade",
        )

    ownership_decl_old = """    bool order_entity_set=false;
    /*
     * Here we assume L1 supports only homogeneous cells with MU = 1
     */"""
    ownership_decl_new = """    bool order_entity_set=false;
    bool slot_map_ul_owns_resources = false;
    bool slot_map_ul_owns_phy_resources = false;
    bool slot_map_dl_owns_phy_resources = false;
    /*
     * Here we assume L1 supports only homogeneous cells with MU = 1
     */"""
    if "bool slot_map_ul_owns_resources = false;" not in api_cpp_text and ownership_decl_old in api_cpp_text:
        api_cpp_text = replace_once(
            api_cpp_text,
            ownership_decl_old,
            ownership_decl_new,
            "cleanup ownership flags",
        )

    ul_aggr_cells_old = """                        if(slot_map_ul->aggrSetCells(cell_ptr, &sc->cells[(*cell_index_list)[cell_index]].params,
                                                    ulbuf_st1,ulbuf_st2,ulbuf_st3_v, rach_occasion, ulbuf_pcap_capture, ulbuf_pcap_capture_ts))
                        {
                            NVLOGE_FMT(TAG, AERIAL_CUPHYDRV_API_EVENT, "SlotMap UL can't set another cell");
                            goto cleanup_err;
                        }
"""
    ul_aggr_cells_new = """                        if(slot_map_ul->aggrSetCells(cell_ptr, &sc->cells[(*cell_index_list)[cell_index]].params,
                                                    ulbuf_st1,ulbuf_st2,ulbuf_st3_v, rach_occasion, ulbuf_pcap_capture, ulbuf_pcap_capture_ts))
                        {
                            NVLOGE_FMT(TAG, AERIAL_CUPHYDRV_API_EVENT, "SlotMap UL can't set another cell");
                            goto cleanup_err;
                        }
                        slot_map_ul_owns_resources = true;
"""
    if "slot_map_ul_owns_resources = true;" not in api_cpp_text and ul_aggr_cells_old in api_cpp_text:
        api_cpp_text = replace_once(
            api_cpp_text,
            ul_aggr_cells_old,
            ul_aggr_cells_new,
            "UL slot map ownership handoff",
        )

    ul_aggr_set_phy_old = """            if(slot_map_ul->aggrSetPhy(aggr_pusch_ptr,aggr_pucch_ptr,aggr_prach_ptr, aggr_srs_ptr,aggr_ulbfw_ptr,current_slot_params_aggr))
            {
                NVLOGE_FMT(TAG, AERIAL_CUPHYDRV_API_EVENT, "SlotMapUL aggrSetPhy");
                goto cleanup_err;
            }"""
    ul_aggr_set_phy_new = """            if(slot_map_ul->aggrSetPhy(aggr_pusch_ptr,aggr_pucch_ptr,aggr_prach_ptr, aggr_srs_ptr,aggr_ulbfw_ptr,current_slot_params_aggr))
            {
                NVLOGE_FMT(TAG, AERIAL_CUPHYDRV_API_EVENT, "SlotMapUL aggrSetPhy");
                goto cleanup_err;
            }
            slot_map_ul_owns_phy_resources = true;"""
    if "slot_map_ul_owns_phy_resources = true;" not in api_cpp_text and ul_aggr_set_phy_old in api_cpp_text:
        api_cpp_text = replace_once(
            api_cpp_text,
            ul_aggr_set_phy_old,
            ul_aggr_set_phy_new,
            "UL aggrSetPhy ownership handoff",
        )

    dl_aggr_set_phy_old = """            if(slot_map_dl->aggrSetPhy(aggr_pdsch_ptr, aggr_pdcch_dl_ptr, aggr_pdcch_ul_ptr, aggr_pbch_ptr, aggr_csirs_ptr,aggr_dlbfw_ptr,current_slot_params_aggr))
            {
                NVLOGE_FMT(TAG, AERIAL_CUPHYDRV_API_EVENT, "SlotMapDL aggrSetPhy");
                goto cleanup_err;
            }"""
    dl_aggr_set_phy_new = """            if(slot_map_dl->aggrSetPhy(aggr_pdsch_ptr, aggr_pdcch_dl_ptr, aggr_pdcch_ul_ptr, aggr_pbch_ptr, aggr_csirs_ptr,aggr_dlbfw_ptr,current_slot_params_aggr))
            {
                NVLOGE_FMT(TAG, AERIAL_CUPHYDRV_API_EVENT, "SlotMapDL aggrSetPhy");
                goto cleanup_err;
            }
            slot_map_dl_owns_phy_resources = true;"""
    if "slot_map_dl_owns_phy_resources = true;" not in api_cpp_text and dl_aggr_set_phy_old in api_cpp_text:
        api_cpp_text = replace_once(
            api_cpp_text,
            dl_aggr_set_phy_old,
            dl_aggr_set_phy_new,
            "DL aggrSetPhy ownership handoff",
        )

    cleanup_old = """    if(slot_map_ul) slot_map_ul->release(UL_MAX_CELLS_PER_SLOT,false);
    if(oentity_ptr) oentity_ptr->release();
    if(slot_map_dl) slot_map_dl->release(DL_MAX_CELLS_PER_SLOT);
    if(dlbuf) dlbuf->release();
    if(current_slot_params) delete current_slot_params;

    if(ulbuf_st1) ulbuf_st1->release();
    // if(ulbuf_st3_v.size() > 0)
    // {
    //     for(auto& p : ulbuf_st3_v)
    //     {
    //         if(p)
    //         {
    //             p->release();
    //             p = nullptr;
    //         }
    //     }
    // }
    // ulbuf_st3_v.clear();

    if(aggr_pusch_ptr) aggr_pusch_ptr->release();
    if(aggr_pucch_ptr) aggr_pucch_ptr->release();
    if(aggr_prach_ptr) aggr_prach_ptr->release();
    if(aggr_srs_ptr)   aggr_srs_ptr->release();
    if(aggr_pdsch_ptr) aggr_pdsch_ptr->release();
"""
    cleanup_new = """    if(slot_map_ul) slot_map_ul->release(UL_MAX_CELLS_PER_SLOT,false);
    if(!slot_map_ul_owns_resources && oentity_ptr) oentity_ptr->release();
    if(slot_map_dl) slot_map_dl->release(DL_MAX_CELLS_PER_SLOT);
    if(dlbuf) dlbuf->release();
    if(current_slot_params) delete current_slot_params;

    if(!slot_map_ul_owns_resources && ulbuf_st1) ulbuf_st1->release();
    // if(ulbuf_st3_v.size() > 0)
    // {
    //     for(auto& p : ulbuf_st3_v)
    //     {
    //         if(p)
    //         {
    //             p->release();
    //             p = nullptr;
    //         }
    //     }
    // }
    // ulbuf_st3_v.clear();

    if(!slot_map_ul_owns_phy_resources && aggr_pusch_ptr) aggr_pusch_ptr->release();
    if(!slot_map_ul_owns_phy_resources && aggr_pucch_ptr) aggr_pucch_ptr->release();
    if(!slot_map_ul_owns_phy_resources && aggr_prach_ptr) aggr_prach_ptr->release();
    if(!slot_map_ul_owns_phy_resources && aggr_srs_ptr)   aggr_srs_ptr->release();
    if(!slot_map_ul_owns_phy_resources && aggr_ulbfw_ptr) aggr_ulbfw_ptr->release();
    if(!slot_map_dl_owns_phy_resources && aggr_pdsch_ptr) aggr_pdsch_ptr->release();
    if(!slot_map_dl_owns_phy_resources && aggr_pdcch_dl_ptr) aggr_pdcch_dl_ptr->release();
    if(!slot_map_dl_owns_phy_resources && aggr_pdcch_ul_ptr) aggr_pdcch_ul_ptr->release();
    if(!slot_map_dl_owns_phy_resources && aggr_pbch_ptr) aggr_pbch_ptr->release();
    if(!slot_map_dl_owns_phy_resources && aggr_csirs_ptr) aggr_csirs_ptr->release();
    if(!slot_map_dl_owns_phy_resources && aggr_dlbfw_ptr) aggr_dlbfw_ptr->release();
"""
    if "if(!slot_map_dl_owns_phy_resources && aggr_pdcch_dl_ptr) aggr_pdcch_dl_ptr->release();" not in api_cpp_text and cleanup_old in api_cpp_text:
        api_cpp_text = replace_once(
            api_cpp_text,
            cleanup_old,
            cleanup_new,
            "cleanup ownership-safe release",
        )

    dl_task_exit_old = """//FIXME: abort the whole DL slot in case of error
exit_error:
    return -1;
}"""
    dl_task_exit_new = """//FIXME: abort the whole DL slot in case of error
exit_error:
    // Ensure the slot cleanup path still runs even if this DL worker exits early.
    slot_map->addSlotEndTask();
    return -1;
}"""
    replace_count = 0
    while dl_task_exit_old in dl_task_cpp_text:
        dl_task_cpp_text = replace_once(
            dl_task_cpp_text,
            dl_task_exit_old,
            dl_task_exit_new,
            f"DL task exit_error slot-end signal {replace_count}",
        )
        replace_count += 1

    control_signal_old = """            {
                NVLOGE_FMT(TAG, AERIAL_CUPHYDRV_API_EVENT, "CSIRS signalCompletion returned error");
                return -1;
            }"""
    control_signal_new = """            {
                NVLOGE_FMT(TAG, AERIAL_CUPHYDRV_API_EVENT, "CSIRS signalCompletion returned error");
                slot_map->addSlotEndTask();
                return -1;
            }"""
    if "CSIRS signalCompletion returned error\");\n                slot_map->addSlotEndTask();" not in dl_task_cpp_text and control_signal_old in dl_task_cpp_text:
        dl_task_cpp_text = replace_once(
            dl_task_cpp_text,
            control_signal_old,
            control_signal_new,
            "control task signalCompletion slot-end signal",
        )

    slot_map_release_old = """    bool dlBfwPrinted = false;
    if(num_active_cells > 0)
    {
        printTimes();
        dlBfwPrinted = true;

        if (aggr_pdsch) {
            aggr_pdsch->cleanup();
            aggr_pdsch->release();
            aggr_pdsch = nullptr;
        }

        if (aggr_pdcch_dl) {
            aggr_pdcch_dl->cleanup();
            aggr_pdcch_dl->release();
            aggr_pdcch_dl = nullptr;
        }

        if (aggr_pdcch_ul) {
            aggr_pdcch_ul->cleanup();
            aggr_pdcch_ul->release();
            aggr_pdcch_ul = nullptr;
        }

        if (aggr_pbch) {
            aggr_pbch->cleanup();
            aggr_pbch->release();
            aggr_pbch = nullptr;
        }

        if (aggr_csirs) {
            aggr_csirs->cleanup();
            aggr_csirs->release();
            aggr_csirs = nullptr;
        }

        aggr_dlbuf_list.clear();
        aggr_cell_list.clear();
        //aggr_slot_info.clear();

        //Valid for all the channels
        // for(int idx = 0; idx < aggr_slot_params.size(); idx++)
        //     aggr_slot_params[idx]->slot_phy_prms.pdsch = nullptr;
        // aggr_slot_params.clear();

        aggr_slot_params = nullptr;
        num_active_cells = 0;
    }

    if(aggr_dlbfw){
        if(!dlBfwPrinted)
        {
            printTimes();
        }
        aggr_dlbfw->cleanup();
        aggr_dlbfw->release();
        aggr_dlbfw = nullptr;
    }
"""
    slot_map_release_new = """    bool dlBfwPrinted = false;
    if(num_active_cells > 0)
    {
        printTimes();
        dlBfwPrinted = true;

        aggr_dlbuf_list.clear();
        aggr_cell_list.clear();
        //aggr_slot_info.clear();

        //Valid for all the channels
        // for(int idx = 0; idx < aggr_slot_params.size(); idx++)
        //     aggr_slot_params[idx]->slot_phy_prms.pdsch = nullptr;
        // aggr_slot_params.clear();

        aggr_slot_params = nullptr;
        num_active_cells = 0;
    }

    auto release_dl_aggr = [](auto*& aggr) {
        if (aggr) {
            aggr->cleanup();
            aggr->release();
            aggr = nullptr;
        }
    };

    release_dl_aggr(aggr_pdsch);
    release_dl_aggr(aggr_pdcch_dl);
    release_dl_aggr(aggr_pdcch_ul);
    release_dl_aggr(aggr_pbch);
    release_dl_aggr(aggr_csirs);

    if(aggr_dlbfw){
        if(!dlBfwPrinted)
        {
            printTimes();
        }
        aggr_dlbfw->cleanup();
        aggr_dlbfw->release();
        aggr_dlbfw = nullptr;
    }
"""
    if "auto release_dl_aggr = [](auto*& aggr)" not in slot_map_dl_cpp_text and slot_map_release_old in slot_map_dl_cpp_text:
        slot_map_dl_cpp_text = replace_once(
            slot_map_dl_cpp_text,
            slot_map_release_old,
            slot_map_release_new,
            "slot_map_dl unconditional aggregator release",
        )

    current_no_peer_count_block = """                else
                {
                    // FAPI-only mode: keep cuPHY work and cleanup, skip FH/C-plane/compression tasks.
                    dl_task_count += 2; // FH callback + DL Task 3 cleanup
                    dl_task_count += aggr_pdsch_ptr ? 1 : 0;
                    dl_task_count += has_dl_control_work ? 1 : 0;
                    dl_task_count += aggr_dlbfw_ptr ? 1 : 0;
                }"""
    if current_no_peer_count_block in api_cpp_text:
        api_cpp_text = replace_once(
            api_cpp_text,
            current_no_peer_count_block,
            """                else
                {
                    // FAPI-only mode: keep cuPHY work and cleanup, skip FH callback/C-plane/compression tasks.
                    dl_task_count += 1; // DL Task 3 cleanup
                    dl_task_count += aggr_pdsch_ptr ? 1 : 0;
                    dl_task_count += has_dl_control_work ? 1 : 0;
                    dl_task_count += aggr_dlbfw_ptr ? 1 : 0;
                }""",
            "dl task_count no-peers cleanup-only",
        )

    current_task2_guard = """                if(!dlbfw_only) //Do not schedule the below DL tasks if DLBFW is the only DL work scheduled
                {
                    ///////////////////////////////////////////////////////////////////////
                    //// Task2: Prepare U-plane, wait DL channels, TX U-plane pkts
                    ///////////////////////////////////////////////////////////////////////"""
    if "if(!dlbfw_only && has_fh_peers)" not in api_cpp_text and current_task2_guard in api_cpp_text:
        api_cpp_text = replace_once(
            api_cpp_text,
            current_task2_guard,
            """                if(!dlbfw_only && has_fh_peers) //No-peers FAPI-only mode skips FH/C-plane/compression tasks
                {
                    ///////////////////////////////////////////////////////////////////////
                    //// Task2: Prepare U-plane, wait DL channels, TX U-plane pkts
                    ///////////////////////////////////////////////////////////////////////""",
            "dl task2 no-peers guard",
        )

    fh_callback_block = """                    // FH callback
                    if(task_index>=dl_task_count)
                    {
                        NVLOGE_FMT(TAG, AERIAL_CUPHYDRV_API_EVENT, "Task index exceeds DL task count");
                        goto cleanup_err;
                    }
                    else
                    {
                        task_dl_ptr_list[task_index] = pdctx->getNextTask();
                        if (!pdctx->getmMIMO_enable())
                        {
                            task_dl_ptr_list[task_index]->init(task_ts_exec[task_index]+ (t_ns)task_index, "TaskDLFHCb", task_work_function_dl_fh_cb, static_cast<void*>(slot_map_dl),
                                                            first_cell, num_cells, dl_task_count, (ENABLE_DL_AFFINITY) ? pdctx->getDLWorkerID(num_dl_workers-dl_worker_offset) : 0);
                        }
                        else
                        {
                            task_dl_ptr_list[task_index]->init(task_ts_exec[task_index]+ (t_ns)task_index, "TaskDLFHCb", task_work_function_dl_fh_cb, static_cast<void*>(slot_map_dl),
                                                            first_cell, num_cells, dl_task_count, (ENABLE_DL_AFFINITY) ? pdctx->getDLWorkerID(num_dl_workers-dl_worker_offset) : 0);
                        }
                        task_index++;
                    }
"""
    if "if(has_fh_peers)\n                    {\n                        // FH callback" not in api_cpp_text and fh_callback_block in api_cpp_text:
        api_cpp_text = replace_once(
            api_cpp_text,
            fh_callback_block,
            """                    if(has_fh_peers)
                    {
                        // FH callback
                        if(task_index>=dl_task_count)
                        {
                            NVLOGE_FMT(TAG, AERIAL_CUPHYDRV_API_EVENT, "Task index exceeds DL task count");
                            goto cleanup_err;
                        }
                        else
                        {
                            task_dl_ptr_list[task_index] = pdctx->getNextTask();
                            if (!pdctx->getmMIMO_enable())
                            {
                                task_dl_ptr_list[task_index]->init(task_ts_exec[task_index]+ (t_ns)task_index, "TaskDLFHCb", task_work_function_dl_fh_cb, static_cast<void*>(slot_map_dl),
                                                                first_cell, num_cells, dl_task_count, (ENABLE_DL_AFFINITY) ? pdctx->getDLWorkerID(num_dl_workers-dl_worker_offset) : 0);
                            }
                            else
                            {
                                task_dl_ptr_list[task_index]->init(task_ts_exec[task_index]+ (t_ns)task_index, "TaskDLFHCb", task_work_function_dl_fh_cb, static_cast<void*>(slot_map_dl),
                                                                first_cell, num_cells, dl_task_count, (ENABLE_DL_AFFINITY) ? pdctx->getDLWorkerID(num_dl_workers-dl_worker_offset) : 0);
                            }
                            task_index++;
                        }
                    }
""",
            "dl fh callback no-peers guard",
        )

    no_peer_ul_guard = """    FhProxy * fhproxy = pdctx->getFhProxy();
    struct rx_order_t * rx_order_item;
    cudaStream_t first_strm;
    pdctx->setUlCtx();"""
    upgraded_no_peer_ul_guard = """    FhProxy * fhproxy = pdctx->getFhProxy();
    struct rx_order_t * rx_order_item;
    cudaStream_t first_strm;
    pdctx->setUlCtx();

    // No-peer FAPI-only mode has no FH RX queues to feed the order kernel.
    if(!fhproxy || !fhproxy->hasRegisteredPeers())
    {
        first_strm = pdctx->getUlOrderStreamPd();
        CUDA_CHECK_PHYDRIVER(cudaEventRecord(start_idle, first_strm));
        CUDA_CHECK_PHYDRIVER(cudaEventRecord(start_order, first_strm));
        CUDA_CHECK_PHYDRIVER(cudaEventRecord(end_order, first_strm));
        if(pdctx->get_ru_type_for_srs_proc() != SINGLE_SECT_MODE)
        {
            auto srs_strm = pdctx->getUlOrderStreamSrsPd();
            CUDA_CHECK_PHYDRIVER(cudaEventRecord(start_idle_srs, srs_strm));
            CUDA_CHECK_PHYDRIVER(cudaEventRecord(start_order_srs, srs_strm));
            CUDA_CHECK_PHYDRIVER(cudaEventRecord(end_order_srs, srs_strm));
        }
        ACCESS_ONCE(*((uint32_t*)start_cuphy_cpu_h->addr())) = 1;
        ACCESS_ONCE(*((uint32_t*)start_cuphy_srs_cpu_h->addr())) = 1;
        for(int idx = 0; idx < UL_MAX_CELLS_PER_SLOT; ++idx)
        {
            ACCESS_ONCE(*((uint32_t*)order_kernel_exit_cond_gdr[idx]->addrh())) = ORDER_KERNEL_EXIT_PRB;
            ACCESS_ONCE(*((uint32_t*)order_kernel_srs_exit_cond_gdr[idx]->addrh())) = ORDER_KERNEL_EXIT_PRB;
        }
        setOrderLaunchedStatus(true);
        setOrderLaunchedStatusSrs(true);
        return 0;
    }"""
    existing_no_peer_ul_guard = """    FhProxy * fhproxy = pdctx->getFhProxy();
    struct rx_order_t * rx_order_item;
    cudaStream_t first_strm;
    pdctx->setUlCtx();

    // No-peer FAPI-only mode has no FH RX queues to feed the order kernel.
    if(!fhproxy || !fhproxy->hasRegisteredPeers())
    {
        setOrderLaunchedStatus(true);
        setOrderLaunchedStatusSrs(true);
        return 0;
    }"""
    if upgraded_no_peer_ul_guard not in order_entity_cpp_text:
        if existing_no_peer_ul_guard in order_entity_cpp_text:
            order_entity_cpp_text = replace_once(
                order_entity_cpp_text,
                existing_no_peer_ul_guard,
                upgraded_no_peer_ul_guard,
                "UL order no-peers guard upgrade",
            )
        else:
            order_entity_cpp_text = replace_once(
                order_entity_cpp_text,
                no_peer_ul_guard,
                upgraded_no_peer_ul_guard,
                "UL order no-peers guard",
            )

    for old, new, label in [
        (
            """float OrderEntity::getGPUIdleTime() {
    return 1000.0f * GetCudaEventElapsedTime(start_idle, start_order, __func__, getId());
}""",
            """float OrderEntity::getGPUIdleTime() {
    PhyDriverCtx* pdctx = StaticConversion<PhyDriverCtx>(pdh).get();
    if(!pdctx->getFhProxy() || !pdctx->getFhProxy()->hasRegisteredPeers()) {
        return 0.0f;
    }
    return 1000.0f * GetCudaEventElapsedTime(start_idle, start_order, __func__, getId());
}""",
            "UL order idle timing no-peers guard",
        ),
        (
            """float OrderEntity::getGPUOrderTime() {
    return 1000.0f * GetCudaEventElapsedTime(start_order, end_order, __func__, getId());
}""",
            """float OrderEntity::getGPUOrderTime() {
    PhyDriverCtx* pdctx = StaticConversion<PhyDriverCtx>(pdh).get();
    if(!pdctx->getFhProxy() || !pdctx->getFhProxy()->hasRegisteredPeers()) {
        return 0.0f;
    }
    return 1000.0f * GetCudaEventElapsedTime(start_order, end_order, __func__, getId());
}""",
            "UL order run timing no-peers guard",
        ),
        (
            """float OrderEntity::getGPUIdleTimeSrs() {
    PhyDriverCtx* pdctx = StaticConversion<PhyDriverCtx>(pdh).get();
    if(pdctx->get_ru_type_for_srs_proc() != SINGLE_SECT_MODE)
    {
        return 1000.0f * GetCudaEventElapsedTime(start_idle_srs, start_order_srs, __func__, getId());
    }
    return 0.0f;
}""",
            """float OrderEntity::getGPUIdleTimeSrs() {
    PhyDriverCtx* pdctx = StaticConversion<PhyDriverCtx>(pdh).get();
    if(!pdctx->getFhProxy() || !pdctx->getFhProxy()->hasRegisteredPeers()) {
        return 0.0f;
    }
    if(pdctx->get_ru_type_for_srs_proc() != SINGLE_SECT_MODE)
    {
        return 1000.0f * GetCudaEventElapsedTime(start_idle_srs, start_order_srs, __func__, getId());
    }
    return 0.0f;
}""",
            "UL order SRS idle timing no-peers guard",
        ),
        (
            """float OrderEntity::getGPUOrderTimeSrs() {
    PhyDriverCtx* pdctx = StaticConversion<PhyDriverCtx>(pdh).get();
    if(pdctx->get_ru_type_for_srs_proc() != SINGLE_SECT_MODE)
    {
        return 1000.0f * GetCudaEventElapsedTime(start_order_srs, end_order_srs, __func__, getId());
    }
    return 0.0f;
}""",
            """float OrderEntity::getGPUOrderTimeSrs() {
    PhyDriverCtx* pdctx = StaticConversion<PhyDriverCtx>(pdh).get();
    if(!pdctx->getFhProxy() || !pdctx->getFhProxy()->hasRegisteredPeers()) {
        return 0.0f;
    }
    if(pdctx->get_ru_type_for_srs_proc() != SINGLE_SECT_MODE)
    {
        return 1000.0f * GetCudaEventElapsedTime(start_order_srs, end_order_srs, __func__, getId());
    }
    return 0.0f;
}""",
            "UL order SRS run timing no-peers guard",
        ),
    ]:
        if new not in order_entity_cpp_text and old in order_entity_cpp_text:
            order_entity_cpp_text = replace_once(order_entity_cpp_text, old, new, label)

    legacy_allowed_latency_init = """    allowed_fapi_latency = 0;

    pdsch_cw_idx_start = 0;

    tx_data_req_meta_data_.num_pdus = 0;
    tx_data_req_meta_data_.data = nullptr;
    tx_data_req_meta_data_.buf = nullptr;

    if (node_config.has_key("prach_ta_offset_usec")) {"""
    relaxed_allowed_latency_init = """    allowed_fapi_latency = 0;

    pdsch_cw_idx_start = 0;

    tx_data_req_meta_data_.num_pdus = 0;
    tx_data_req_meta_data_.data = nullptr;
    tx_data_req_meta_data_.buf = nullptr;

    allowed_fapi_latency = 20;
    if (node_config.has_key("allowed_fapi_latency")) {
        allowed_fapi_latency = node_config["allowed_fapi_latency"].as<uint32_t>();
    }
    NVLOGI_FMT(TAG, "scf_5g_fapi::phy::phy(): allowed_fapi_latency={}", allowed_fapi_latency);
    if (node_config.has_key("prach_ta_offset_usec")) {"""
    if "allowed_fapi_latency = 20;" not in scf_phy_cpp_text and legacy_allowed_latency_init in scf_phy_cpp_text:
        scf_phy_cpp_text = replace_once(
            scf_phy_cpp_text,
            legacy_allowed_latency_init,
            relaxed_allowed_latency_init,
            "allowed_fapi_latency constructor init",
        )

    strict_l2_slt_check = """#ifdef ENABLE_L2_SLT_RSP
int phy::check_sfn_slot(int cell_id, int msg_id, sfn_slot_t ss_msg)
{
    sfn_slot_t& ss_curr = phy_module().get_curr_sfn_slot();
    uint32_t fapi_latency = phy_module().get_fapi_latency(ss_msg);

    if ((msg_id != SCF_FAPI_SLOT_INDICATION) && (ss_curr.u32 != ss_msg.u32))
    {
        NVLOGW_FMT(TAG, "{}: SFN mismatch cell_id={} expected={}.{} received={}.{} msg_id=0x{:02X}", __FUNCTION__,
                   cell_id, ss_curr.u16.sfn, ss_curr.u16.slot, ss_msg.u16.sfn, ss_msg.u16.slot, msg_id);
        switch(msg_id)
        {
            case SCF_FAPI_DL_TTI_REQUEST:
            case SCF_FAPI_UL_TTI_REQUEST:
                send_error_indication(static_cast<scf_fapi_message_id_e>(msg_id), SCF_ERROR_CODE_SFN_OUT_OF_SYNC, ss_msg.u16.sfn, ss_msg.u16.slot);
                break;
            case SCF_FAPI_UL_DCI_REQUEST:
            case SCF_FAPI_TX_DATA_REQUEST:
                send_error_indication(static_cast<scf_fapi_message_id_e>(msg_id), SCF_ERROR_CODE_MSG_INVALID_SFN, ss_msg.u16.sfn, ss_msg.u16.slot);
                break;
            default:
                break;
        }
        return -1;
    }
    return 0;
}"""
    relaxed_l2_slt_check = """#ifdef ENABLE_L2_SLT_RSP
int phy::check_sfn_slot(int cell_id, int msg_id, sfn_slot_t ss_msg)
{
    sfn_slot_t& ss_curr = phy_module().get_curr_sfn_slot();
    uint32_t fapi_latency = phy_module().get_fapi_latency(ss_msg);

    if ((msg_id != SCF_FAPI_SLOT_INDICATION) && (ss_curr.u32 != ss_msg.u32) && (fapi_latency > allowed_fapi_latency))
    {
        NVLOGW_FMT(TAG, "{}: SFN mismatch cell_id={} expected={}.{} received={}.{} msg_id=0x{:02X} latency={} dropped", __FUNCTION__,
                   cell_id, ss_curr.u16.sfn, ss_curr.u16.slot, ss_msg.u16.sfn, ss_msg.u16.slot, msg_id, fapi_latency);
        switch(msg_id)
        {
            case SCF_FAPI_DL_TTI_REQUEST:
            case SCF_FAPI_UL_TTI_REQUEST:
                send_error_indication(static_cast<scf_fapi_message_id_e>(msg_id), SCF_ERROR_CODE_SFN_OUT_OF_SYNC, ss_msg.u16.sfn, ss_msg.u16.slot);
                break;
            case SCF_FAPI_UL_DCI_REQUEST:
            case SCF_FAPI_TX_DATA_REQUEST:
                send_error_indication(static_cast<scf_fapi_message_id_e>(msg_id), SCF_ERROR_CODE_MSG_INVALID_SFN, ss_msg.u16.sfn, ss_msg.u16.slot);
                break;
            default:
                break;
        }
        return -1;
    }
    return 0;
}"""
    if "latency={} dropped" not in scf_phy_cpp_text and strict_l2_slt_check in scf_phy_cpp_text:
        scf_phy_cpp_text = replace_once(
            scf_phy_cpp_text,
            strict_l2_slt_check,
            relaxed_l2_slt_check,
            "ENABLE_L2_SLT_RSP relaxed latency check",
        )

    pusch_parse_log_old = """            ue.ndi = data->new_data_indicator;
            ue.harqProcessId = data->harq_process_id;
            next += sizeof(scf_fapi_pusch_data_t);"""
    pusch_parse_log_nvlog = """            ue.ndi = data->new_data_indicator;
            ue.harqProcessId = data->harq_process_id;
            static uint32_t pusch_parse_debug_count = 0;
            if (pusch_parse_debug_count < 128) {
                NVLOGI_FMT(TAG,
                           "FAPI PUSCH parse SFN {}.{} rnti={} harqProcessId={} rv={} ndi={} tbSize={} startPrb={} nPrb={} startSym={} nSym={}",
                           slotinfo.sfn_,
                           slotinfo.slot_,
                           static_cast<unsigned>(ue.rnti),
                           static_cast<unsigned>(ue.harqProcessId),
                           static_cast<unsigned>(ue.rv),
                           static_cast<unsigned>(ue.ndi),
                           static_cast<unsigned>(ue.TBSize),
                           static_cast<unsigned>(ue_grp.startPrb),
                           static_cast<unsigned>(ue_grp.nPrb),
                           static_cast<unsigned>(ue_grp.puschStartSym),
                           static_cast<unsigned>(ue_grp.nPuschSym));
                pusch_parse_debug_count++;
            }
            next += sizeof(scf_fapi_pusch_data_t);"""
    pusch_parse_log_stderr = """            ue.ndi = data->new_data_indicator;
            ue.harqProcessId = data->harq_process_id;
            static uint32_t pusch_parse_debug_count = 0;
            if (pusch_parse_debug_count < 128) {
                std::fprintf(stderr,
                             "FAPI PUSCH parse SFN %u.%u rnti=%u harqProcessId=%u rv=%u ndi=%u tbSize=%u startPrb=%u nPrb=%u startSym=%u nSym=%u\\n",
                             static_cast<unsigned>(slotinfo.sfn_),
                             static_cast<unsigned>(slotinfo.slot_),
                             static_cast<unsigned>(ue.rnti),
                             static_cast<unsigned>(ue.harqProcessId),
                             static_cast<unsigned>(ue.rv),
                             static_cast<unsigned>(ue.ndi),
                             static_cast<unsigned>(ue.TBSize),
                             static_cast<unsigned>(ue_grp.startPrb),
                             static_cast<unsigned>(ue_grp.nPrb),
                             static_cast<unsigned>(ue_grp.puschStartSym),
                             static_cast<unsigned>(ue_grp.nPuschSym));
                std::fflush(stderr);
                pusch_parse_debug_count++;
            }
            next += sizeof(scf_fapi_pusch_data_t);"""
    if pusch_parse_log_stderr not in slot_commands_cpp_text:
        if pusch_parse_log_nvlog in slot_commands_cpp_text:
            slot_commands_cpp_text = replace_once(
                slot_commands_cpp_text,
                pusch_parse_log_nvlog,
                pusch_parse_log_stderr,
                "PUSCH parse debug log stderr upgrade",
            )
        elif pusch_parse_log_old in slot_commands_cpp_text:
            slot_commands_cpp_text = replace_once(
                slot_commands_cpp_text,
                pusch_parse_log_old,
                pusch_parse_log_stderr,
                "PUSCH parse debug log",
            )

    crc_emit_log_old = """            crcInfo.rnti = params.ue_info[i].rnti;
            crcInfo.harq_id = params.ue_info[i].harqProcessId; // Update once we know what this is
#ifdef SCF_FAPI_10_04"""
    crc_emit_log_nvlog = """            crcInfo.rnti = params.ue_info[i].rnti;
            crcInfo.harq_id = params.ue_info[i].harqProcessId; // Update once we know what this is
            static uint32_t crc_emit_debug_count = 0;
            if (crc_emit_debug_count < 128) {
                NVLOGI_FMT(TAG,
                           "FAPI CRC emit SFN {}.{} rnti={} harq_id={} tb_crc_status={} handle={}",
                           static_cast<unsigned>(indication.sfn),
                           static_cast<unsigned>(indication.slot),
                           static_cast<unsigned>(crcInfo.rnti),
                           static_cast<unsigned>(crcInfo.harq_id),
                           static_cast<unsigned>(crcInfo.tb_crc_status),
                           static_cast<unsigned>(crcInfo.handle));
                crc_emit_debug_count++;
            }
#ifdef SCF_FAPI_10_04"""
    crc_emit_log_stderr = """            crcInfo.rnti = params.ue_info[i].rnti;
            crcInfo.harq_id = params.ue_info[i].harqProcessId; // Update once we know what this is
            static uint32_t crc_emit_debug_count = 0;
            if (crc_emit_debug_count < 128) {
                std::fprintf(stderr,
                             "FAPI CRC emit SFN %u.%u rnti=%u harq_id=%u tb_crc_status=%u handle=%u\\n",
                             static_cast<unsigned>(indication.sfn),
                             static_cast<unsigned>(indication.slot),
                             static_cast<unsigned>(crcInfo.rnti),
                             static_cast<unsigned>(crcInfo.harq_id),
                             static_cast<unsigned>(crcInfo.tb_crc_status),
                             static_cast<unsigned>(crcInfo.handle));
                std::fflush(stderr);
                crc_emit_debug_count++;
            }
#ifdef SCF_FAPI_10_04"""
    if crc_emit_log_stderr not in scf_phy_cpp_text:
        if crc_emit_log_nvlog in scf_phy_cpp_text:
            scf_phy_cpp_text = replace_once(
                scf_phy_cpp_text,
                crc_emit_log_nvlog,
                crc_emit_log_stderr,
                "CRC emit debug log stderr upgrade",
            )
        elif crc_emit_log_old in scf_phy_cpp_text:
            scf_phy_cpp_text = replace_once(
                scf_phy_cpp_text,
                crc_emit_log_old,
                crc_emit_log_stderr,
                "CRC emit debug log",
            )

    context_path.write_text(context_text)
    fh_hpp_path.write_text(fh_hpp_text)
    constant_hpp_path.write_text(constant_hpp_text)
    fh_cpp_path.write_text(fh_cpp_text)
    api_cpp_path.write_text(api_cpp_text)
    dl_task_cpp_path.write_text(dl_task_cpp_text)
    slot_map_dl_cpp_path.write_text(slot_map_dl_cpp_text)
    order_entity_cpp_path.write_text(order_entity_cpp_text)
    slot_commands_cpp_path.write_text(slot_commands_cpp_text)
    scf_phy_cpp_path.write_text(scf_phy_cpp_text)
    print(f"patched {context_path}")
    print(f"patched {fh_hpp_path}")
    print(f"patched {constant_hpp_path}")
    print(f"patched {fh_cpp_path}")
    print(f"patched {api_cpp_path}")
    print(f"patched {dl_task_cpp_path}")
    print(f"patched {slot_map_dl_cpp_path}")
    print(f"patched {order_entity_cpp_path}")
    print(f"patched {slot_commands_cpp_path}")
    print(f"patched {scf_phy_cpp_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
