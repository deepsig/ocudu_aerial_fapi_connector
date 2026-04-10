// SPDX-FileCopyrightText: Copyright (c) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
// Aerial FAPI Bridge — RX Thread

#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace ocudu::fapi {
class error_indication_notifier;
class p5_responses_notifier;
class p7_indications_notifier;
class p7_slot_indication_notifier;
} // namespace ocudu::fapi

namespace aerial_bridge {

class nvipc_transport;

/// Configuration for the RX polling thread.
struct rx_thread_config {
  int      cpu_core   = -1; ///< CPU core affinity (-1 = no pinning).
  int      priority   = 90; ///< SCHED_FIFO priority.
  unsigned numerology = 1;  ///< Subcarrier spacing numerology (0=15kHz, 1=30kHz, ...).
};

/// Dedicated thread that polls nvIPC for L1 indications and dispatches them
/// to the appropriate OCUDU notifier interfaces.
class rx_thread
{
public:
  explicit rx_thread(nvipc_transport& transport);
  ~rx_thread();

  // ── Notifier registration (called by sector adaptor) ──────────

  void set_slot_indication_notifier(ocudu::fapi::p7_slot_indication_notifier* n);
  void set_indications_notifier(ocudu::fapi::p7_indications_notifier* n);
  void set_error_notifier(ocudu::fapi::error_indication_notifier* n);
  void set_p5_responses_notifier(ocudu::fapi::p5_responses_notifier* n);

  // ── Lifecycle ─────────────────────────────────────────────────

  void start(const rx_thread_config& cfg);
  void stop();

private:
  void run_loop();
  void dispatch_message(int32_t msg_id, int32_t cell_id, void* msg_buf, int32_t msg_len,
                        void* data_buf, int32_t data_len);

  nvipc_transport& transport_;
  rx_thread_config cfg_;

  std::atomic<bool> running_{false};
  std::thread       thread_;

  // OCUDU notifiers (set before start, read from RX thread).
  ocudu::fapi::p7_slot_indication_notifier* slot_notifier_  = nullptr;
  ocudu::fapi::p7_indications_notifier*     ind_notifier_   = nullptr;
  ocudu::fapi::error_indication_notifier*   error_notifier_ = nullptr;
  ocudu::fapi::p5_responses_notifier*       p5_notifier_    = nullptr;
};

} // namespace aerial_bridge
