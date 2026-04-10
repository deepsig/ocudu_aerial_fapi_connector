// SPDX-FileCopyrightText: Copyright (c) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
// Aerial FAPI Bridge — nvIPC Transport Wrapper

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

// nvIPC types — include directly to avoid forward-declaration conflicts
// with the C typedef in nv_ipc.h.
extern "C" {
#include "nv_ipc.h"
}

namespace aerial_bridge {

/// Configuration for the nvIPC transport layer.
struct nvipc_transport_config {
  std::string prefix    = "nvipc"; ///< SHM prefix (must match L1 config).
  std::string yaml_path;           ///< Optional: path to nvipc YAML config file.
};

/// Wraps the nvIPC shared-memory transport lifecycle.
///
/// Connects to the Aerial L1 as a secondary (MAC-side) nvIPC client.
/// Provides buffer allocation, message send/receive, and semaphore operations.
class nvipc_transport
{
public:
  nvipc_transport();
  ~nvipc_transport();

  /// Connect to the L1's nvIPC shared memory region.
  /// @return true on success.
  bool connect(const nvipc_transport_config& cfg);

  /// Disconnect and release resources.
  void disconnect();

  /// @return true if connected to L1.
  bool is_connected() const;

  // ── TX path (OCUDU → Aerial) ──────────────────────────────────

  /// Allocate a TX message buffer from the CPU_MSG pool.
  /// Sets msg.msg_buf; optionally also msg.data_buf if data_len > 0.
  /// @return 0 on success, <0 on failure.
  int tx_allocate(nv_ipc_msg_t& msg, uint32_t options = 0);

  /// Send a previously allocated TX message to L1.
  void tx_send(nv_ipc_msg_t& msg);

  /// Release a TX buffer without sending (error path).
  void tx_release(nv_ipc_msg_t& msg);

  /// Post the TX TTI semaphore to signal L1 that all slot messages are sent.
  void tx_sem_post();

  // ── RX path (Aerial → OCUDU) ──────────────────────────────────

  /// Block until L1 posts a TTI semaphore (new slot available).
  void rx_sem_wait();

  /// Receive the next queued message from L1.
  /// @return 0 on success, <0 if no more messages.
  int rx_recv(nv_ipc_msg_t& msg);

  /// Release a received RX buffer back to the pool.
  void rx_release(nv_ipc_msg_t& msg);

  /// @return raw nvIPC handle (for advanced use).
  nv_ipc_t* raw() const { return ipc_; }

private:
  nv_ipc_t*         ipc_       = nullptr;
  std::atomic<bool> connected_ = false;
};

} // namespace aerial_bridge
