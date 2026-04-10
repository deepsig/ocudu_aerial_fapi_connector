// SPDX-FileCopyrightText: Copyright (c) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
// Aerial FAPI Bridge — Main FAPI Adaptor (phy_fapi_adaptor implementation)

#pragma once

#include "aerial_bridge/nvipc_transport.h"
#include "aerial_bridge/rx_thread.h"
#include "ocudu/fapi/p5/p5_requests_gateway.h"
#include "ocudu/fapi/p7/p7_last_request_notifier.h"
#include "ocudu/fapi/p7/p7_requests_gateway.h"
#include "ocudu/fapi_adaptor/phy/p5/phy_fapi_p5_sector_adaptor.h"
#include "ocudu/fapi_adaptor/phy/p7/phy_fapi_p7_sector_adaptor.h"
#include "ocudu/fapi_adaptor/phy/phy_fapi_adaptor.h"
#include "ocudu/ran/slot_point.h"

#include <memory>
#include <vector>

namespace aerial_bridge {

// ─────────────────────────────────────────────────────────────────
// P5 Requests Gateway — translates OCUDU config/start/stop to SCF
// ─────────────────────────────────────────────────────────────────
class aerial_p5_gateway final : public ocudu::fapi::p5_requests_gateway
{
public:
  explicit aerial_p5_gateway(nvipc_transport& transport);

  void send_param_request(const ocudu::fapi::param_request& msg) override;
  void send_config_request(const ocudu::fapi::config_request& msg) override;
  void send_start_request(const ocudu::fapi::start_request& msg) override;
  void send_stop_request(const ocudu::fapi::stop_request& msg) override;

private:
  nvipc_transport& transport_;
};

// ─────────────────────────────────────────────────────────────────
// P7 Requests Gateway — translates OCUDU DL/UL scheduling to SCF
// ─────────────────────────────────────────────────────────────────
class aerial_p7_gateway final : public ocudu::fapi::p7_requests_gateway
{
public:
  explicit aerial_p7_gateway(nvipc_transport& transport);

  void send_dl_tti_request(const ocudu::fapi::dl_tti_request& msg) override;
  void send_ul_tti_request(const ocudu::fapi::ul_tti_request& msg) override;
  void send_ul_dci_request(const ocudu::fapi::ul_dci_request& msg) override;
  void send_tx_data_request(const ocudu::fapi::tx_data_request& msg) override;

private:
  nvipc_transport& transport_;
};

// ─────────────────────────────────────────────────────────────────
// P7 Last Request Notifier — signals L1 that all slot messages sent
// ─────────────────────────────────────────────────────────────────
class aerial_last_request_notifier final : public ocudu::fapi::p7_last_request_notifier
{
public:
  explicit aerial_last_request_notifier(nvipc_transport& transport);

  void on_last_message(ocudu::slot_point slot) override;

private:
  nvipc_transport& transport_;
};

// ─────────────────────────────────────────────────────────────────
// P5 Sector Adaptor
// ─────────────────────────────────────────────────────────────────
class aerial_p5_adaptor final : public ocudu::fapi_adaptor::phy_fapi_p5_sector_adaptor
{
public:
  explicit aerial_p5_adaptor(nvipc_transport& transport, rx_thread& rx);

  ocudu::fapi::p5_requests_gateway& get_p5_requests_gateway() override;
  void set_p5_responses_notifier(ocudu::fapi::p5_responses_notifier& notifier) override;
  void set_error_indication_notifier(ocudu::fapi::error_indication_notifier& notifier) override;

private:
  aerial_p5_gateway p5_gateway_;
  rx_thread&        rx_;
};

// ─────────────────────────────────────────────────────────────────
// P7 Sector Adaptor
// ─────────────────────────────────────────────────────────────────
class aerial_p7_adaptor final : public ocudu::fapi_adaptor::phy_fapi_p7_sector_adaptor
{
public:
  explicit aerial_p7_adaptor(nvipc_transport& transport, rx_thread& rx);

  ocudu::fapi::p7_requests_gateway&      get_p7_requests_gateway() override;
  ocudu::fapi::p7_last_request_notifier& get_p7_last_request_notifier() override;
  void set_p7_slot_indication_notifier(ocudu::fapi::p7_slot_indication_notifier& notifier) override;
  void set_error_indication_notifier(ocudu::fapi::error_indication_notifier& notifier) override;
  void set_p7_indications_notifier(ocudu::fapi::p7_indications_notifier& notifier) override;

private:
  aerial_p7_gateway               p7_gateway_;
  aerial_last_request_notifier    last_req_notifier_;
  rx_thread&                      rx_;
};

// ─────────────────────────────────────────────────────────────────
// Sector Adaptor (combines P5 + P7)
// ─────────────────────────────────────────────────────────────────
class aerial_sector_adaptor final : public ocudu::fapi_adaptor::phy_fapi_sector_adaptor
{
public:
  aerial_sector_adaptor(nvipc_transport& transport, rx_thread& rx);

  ocudu::fapi_adaptor::phy_fapi_p5_sector_adaptor& get_p5_sector_adaptor() override;
  ocudu::fapi_adaptor::phy_fapi_p7_sector_adaptor& get_p7_sector_adaptor() override;
  void start() override;
  void stop() override;

private:
  aerial_p5_adaptor p5_;
  aerial_p7_adaptor p7_;
};

// ─────────────────────────────────────────────────────────────────
// Main FAPI Adaptor (top-level, owns transport + sectors)
// ─────────────────────────────────────────────────────────────────
class aerial_fapi_adaptor final : public ocudu::fapi_adaptor::phy_fapi_adaptor
{
public:
  /// Construct the adaptor with the given nvIPC and RX thread configuration.
  aerial_fapi_adaptor(const nvipc_transport_config& nvipc_cfg,
                      const rx_thread_config&       rx_cfg,
                      unsigned                      num_cells = 1);

  ~aerial_fapi_adaptor() override;

  ocudu::fapi_adaptor::phy_fapi_sector_adaptor& get_sector_adaptor(unsigned cell_id) override;

private:
  nvipc_transport                              transport_;
  rx_thread                                    rx_;
  std::vector<std::unique_ptr<aerial_sector_adaptor>> sectors_;
};

} // namespace aerial_bridge
