// SPDX-FileCopyrightText: Copyright (c) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
// Aerial FAPI Bridge — Split 6 Plugin Implementation

#include "aerial_bridge/aerial_split6_plugin.h"
#include "aerial_bridge/aerial_fapi_adaptor.h"

#include "ocudu/du/du_high/du_high_configuration.h"
#include "split6_o_du_low_fapi_adaptor_configuration.h"

#include <CLI/CLI.hpp>
#include <cstdio>

namespace aerial_bridge {

aerial_split6_plugin::aerial_split6_plugin()
{
  // Set defaults.
  cfg_.nvipc.prefix = "nvipc";
  cfg_.rx.cpu_core  = -1;
  cfg_.rx.priority  = 90;
  cfg_.rx.numerology = 1;
}

aerial_split6_plugin::~aerial_split6_plugin() = default;

void aerial_split6_plugin::on_parsing_configuration_registration(CLI::App& app)
{
  // Register CLI11 options for the Aerial bridge configuration.
  auto* aerial = app.add_subcommand("aerial", "Aerial L1 bridge configuration")->fallthrough();

  aerial->add_option("--nvipc_prefix", cfg_.nvipc.prefix,
                     "nvIPC shared memory prefix (must match L1 config)")
      ->default_val("nvipc");

  aerial->add_option("--nvipc_yaml", cfg_.nvipc.yaml_path,
                     "Path to nvIPC YAML config file (optional)");

  aerial->add_option("--rx_cpu_core", cfg_.rx.cpu_core,
                     "CPU core for RX polling thread (-1 = no pinning)")
      ->default_val(-1);

  aerial->add_option("--rx_priority", cfg_.rx.priority,
                     "SCHED_FIFO priority for RX thread")
      ->default_val(90);

  aerial->add_option("--numerology", cfg_.rx.numerology,
                     "Subcarrier spacing numerology (0=15kHz, 1=30kHz, 2=60kHz)")
      ->default_val(1);
}

bool aerial_split6_plugin::on_configuration_validation() const
{
  if (cfg_.nvipc.prefix.empty()) {
    std::fprintf(stderr, "[aerial_bridge] nvIPC prefix must not be empty\n");
    return false;
  }
  if (cfg_.rx.numerology > 4) {
    std::fprintf(stderr, "[aerial_bridge] Invalid numerology %u (must be 0-4)\n", cfg_.rx.numerology);
    return false;
  }
  return true;
}

bool aerial_split6_plugin::is_ran_config_supported(
    const ocudu::odu::du_high_ran_config& /*configuration*/) const
{
  // Accept any RAN configuration for now.
  // Phase 4: validate against L1 capabilities (max bandwidth, antenna count, etc.)
  return true;
}

void aerial_split6_plugin::on_loggers_registration()
{
  // Phase 4: register custom loggers for the bridge.
}

std::unique_ptr<ocudu::fapi_adaptor::phy_fapi_adaptor>
aerial_split6_plugin::create_fapi_adaptor(
    const ocudu::fapi_adaptor::split6_o_du_low_fapi_adaptor_configuration& fapi_cfg,
    const ocudu::o_du_unit_dependencies& /*dependencies*/)
{
  unsigned num_cells = fapi_cfg.cells.size();
  if (num_cells == 0) {
    num_cells = 1;
  }

  // Use numerology from the first cell's SCS if available.
  if (!fapi_cfg.cells.empty()) {
    cfg_.rx.numerology = static_cast<unsigned>(fapi_cfg.cells[0].scs_common);
  }

  std::fprintf(stdout, "[aerial_bridge] Creating FAPI adaptor for %u cell(s), "
                       "nvIPC prefix=%s, numerology=%u\n",
               num_cells, cfg_.nvipc.prefix.c_str(), cfg_.rx.numerology);

  return std::make_unique<aerial_fapi_adaptor>(cfg_.nvipc, cfg_.rx, num_cells);
}

void aerial_split6_plugin::fill_worker_manager_config(ocudu::worker_manager_config& /*config*/)
{
  // The bridge doesn't need additional worker threads from OCUDU's worker manager.
  // The RX thread is managed internally.
}

} // namespace aerial_bridge

// ═══════════════════════════════════════════════════════════════
// Plugin factory function — replaces the dummy when compiled with
// OCUDU_HAS_ENTERPRISE or linked in place of the dummy.
// ═══════════════════════════════════════════════════════════════

#ifdef AERIAL_BRIDGE_REGISTER_PLUGIN
std::unique_ptr<ocudu::split6_plugin> ocudu::create_split6_plugin(std::string_view /*app_name*/)
{
  return std::make_unique<aerial_bridge::aerial_split6_plugin>();
}
#endif
