// SPDX-FileCopyrightText: Copyright (c) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
// Aerial FAPI Bridge — nvIPC Transport Implementation

#include "aerial_bridge/nvipc_transport.h"

#include <cstring>
#include <stdexcept>

namespace aerial_bridge {

nvipc_transport::nvipc_transport() = default;

nvipc_transport::~nvipc_transport()
{
  disconnect();
}

bool nvipc_transport::connect(const nvipc_transport_config& cfg)
{
  if (connected_) {
    return true;
  }

  nv_ipc_config_t ipc_cfg;
  std::memset(&ipc_cfg, 0, sizeof(ipc_cfg));

  // If a YAML config file is provided, load it.
  if (!cfg.yaml_path.empty()) {
    if (load_nv_ipc_yaml_config(&ipc_cfg, cfg.yaml_path.c_str(), NV_IPC_MODULE_MAC) < 0) {
      return false;
    }
  } else {
    // Minimal secondary SHM config — only prefix and transport type needed.
    // The primary (L1) saves full config to shared memory; secondary reads it.
    ipc_cfg.module_type   = NV_IPC_MODULE_MAC;
    ipc_cfg.ipc_transport = NV_IPC_TRANSPORT_SHM;
    std::strncpy(ipc_cfg.transport_config.shm.prefix, cfg.prefix.c_str(),
                 NV_NAME_MAX_LEN - 1);
  }

  ipc_ = create_nv_ipc_interface(&ipc_cfg);
  if (!ipc_) {
    return false;
  }

  connected_ = true;
  return true;
}

void nvipc_transport::disconnect()
{
  if (ipc_) {
    ipc_->ipc_destroy(ipc_);
    ipc_       = nullptr;
    connected_ = false;
  }
}

bool nvipc_transport::is_connected() const
{
  return connected_;
}

// ── TX path ──────────────────────────────────────────────────────

int nvipc_transport::tx_allocate(nv_ipc_msg_t& msg, uint32_t options)
{
  return ipc_->tx_allocate(ipc_, &msg, options);
}

void nvipc_transport::tx_send(nv_ipc_msg_t& msg)
{
  ipc_->tx_send_msg(ipc_, &msg);
}

void nvipc_transport::tx_release(nv_ipc_msg_t& msg)
{
  ipc_->tx_release(ipc_, &msg);
}

void nvipc_transport::tx_sem_post()
{
  ipc_->tx_tti_sem_post(ipc_);
}

// ── RX path ──────────────────────────────────────────────────────

void nvipc_transport::rx_sem_wait()
{
  ipc_->rx_tti_sem_wait(ipc_);
}

int nvipc_transport::rx_recv(nv_ipc_msg_t& msg)
{
  return ipc_->rx_recv_msg(ipc_, &msg);
}

void nvipc_transport::rx_release(nv_ipc_msg_t& msg)
{
  ipc_->rx_release(ipc_, &msg);
}

} // namespace aerial_bridge
