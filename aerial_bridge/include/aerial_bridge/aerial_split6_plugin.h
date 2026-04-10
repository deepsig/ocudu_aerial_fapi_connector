// SPDX-FileCopyrightText: Copyright (c) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
// Aerial FAPI Bridge — Split 6 Plugin

#pragma once

#include "aerial_bridge/nvipc_transport.h"
#include "aerial_bridge/rx_thread.h"

// OCUDU split6 plugin interface.
#include "split6_plugin.h"

namespace aerial_bridge {

/// Configuration parsed from CLI / YAML for the Aerial bridge.
struct aerial_bridge_config {
  nvipc_transport_config nvipc;
  rx_thread_config       rx;
};

/// Split 6 plugin implementation that bridges OCUDU L2 to Aerial L1 via nvIPC.
class aerial_split6_plugin final : public ocudu::split6_plugin
{
public:
  aerial_split6_plugin();
  ~aerial_split6_plugin() override;

  void on_parsing_configuration_registration(CLI::App& app) override;
  bool on_configuration_validation() const override;
  bool is_ran_config_supported(const ocudu::odu::du_high_ran_config& configuration) const override;
  void on_loggers_registration() override;

  std::unique_ptr<ocudu::fapi_adaptor::phy_fapi_adaptor>
  create_fapi_adaptor(const ocudu::fapi_adaptor::split6_o_du_low_fapi_adaptor_configuration& fapi_cfg,
                      const ocudu::o_du_unit_dependencies& dependencies) override;

  void fill_worker_manager_config(ocudu::worker_manager_config& config) override;

private:
  aerial_bridge_config cfg_;
};

} // namespace aerial_bridge
