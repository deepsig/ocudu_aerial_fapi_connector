// SPDX-FileCopyrightText: Copyright (c) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
// Aerial FAPI Bridge — RX Thread Implementation

#include "aerial_bridge/rx_thread.h"
#include "aerial_bridge/msg_translator.h"
#include "aerial_bridge/nvipc_transport.h"

extern "C" {
#include "nv_ipc.h"
}

// OCUDU FAPI interfaces.
#include "ocudu/fapi/common/error_code.h"
#include "ocudu/fapi/common/error_indication.h"
#include "ocudu/fapi/common/error_indication_notifier.h"
#include "ocudu/fapi/p5/p5_messages.h"
#include "ocudu/fapi/p5/p5_responses_notifier.h"
#include "ocudu/fapi/p7/p7_indications_notifier.h"
#include "ocudu/fapi/p7/p7_slot_indication_notifier.h"

// OCUDU FAPI message types.
#include "ocudu/fapi/p7/messages/slot_indication.h"
#include "ocudu/ran/slot_point.h"
#include "ocudu/ran/slot_point_extended.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <pthread.h>
#include <sched.h>

namespace aerial_bridge {

static ocudu::fapi::error_code_id to_error_code(uint8_t code)
{
  return static_cast<ocudu::fapi::error_code_id>(code);
}

static ocudu::fapi::message_type_id to_message_type(uint8_t code)
{
  return static_cast<ocudu::fapi::message_type_id>(code);
}

rx_thread::rx_thread(nvipc_transport& transport)
  : transport_(transport)
{
}

rx_thread::~rx_thread()
{
  stop();
}

void rx_thread::set_slot_indication_notifier(ocudu::fapi::p7_slot_indication_notifier* n)
{
  slot_notifier_ = n;
}

void rx_thread::set_indications_notifier(ocudu::fapi::p7_indications_notifier* n)
{
  ind_notifier_ = n;
}

void rx_thread::set_error_notifier(ocudu::fapi::error_indication_notifier* n)
{
  error_notifier_ = n;
}

void rx_thread::set_p5_responses_notifier(ocudu::fapi::p5_responses_notifier* n)
{
  p5_notifier_ = n;
}

void rx_thread::start(const rx_thread_config& cfg)
{
  if (running_) {
    return;
  }
  cfg_     = cfg;
  running_ = true;
  thread_  = std::thread(&rx_thread::run_loop, this);

  // Set thread name.
  pthread_setname_np(thread_.native_handle(), "aerial_rx");

  // Set CPU affinity if configured.
  if (cfg_.cpu_core >= 0) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cfg_.cpu_core, &cpuset);
    pthread_setaffinity_np(thread_.native_handle(), sizeof(cpuset), &cpuset);
  }

  // Set real-time priority if configured.
  if (cfg_.priority > 0) {
    struct sched_param param;
    param.sched_priority = cfg_.priority;
    pthread_setschedparam(thread_.native_handle(), SCHED_FIFO, &param);
  }
}

void rx_thread::stop()
{
  if (!running_) {
    return;
  }
  running_ = false;
  // The thread may be blocked on rx_sem_wait. In practice we'd need to
  // interrupt it (e.g., via a cancel or by posting a dummy semaphore).
  // For now, detach to avoid blocking on join during shutdown.
  if (thread_.joinable()) {
    thread_.detach();
  }
}

void rx_thread::run_loop()
{
  nv_ipc_msg_t msg;
  while (running_) {
    // Block until L1 signals a new TTI.
    transport_.rx_sem_wait();

    if (!running_) break;

    // Drain all queued messages for this TTI.
    while (transport_.rx_recv(msg) == 0) {
      dispatch_message(msg.msg_id, msg.cell_id, msg.msg_buf, msg.msg_len,
                       msg.data_buf, msg.data_len);
      transport_.rx_release(msg);
    }
  }
}

void rx_thread::dispatch_message(int32_t msg_id, int32_t cell_id, void* msg_buf, int32_t msg_len,
                                 void* data_buf, int32_t data_len)
{
  // Validate buffer pointer and length before any processing.
  if (!msg_buf || msg_len <= 0 || msg_len > 1000000) {
    return;
  }

  switch (msg_id) {
    case scf_msg_id::SLOT_INDICATION: {
      if (!slot_notifier_ || !msg_buf || msg_len < 12) break;

      // Copy the header bytes locally to avoid stale buffer access.
      // SLOT_IND only needs 12 bytes: header(2) + body_header(6) + sfn(2) + slot(2).
      uint8_t local_buf[16];
      size_t copy_len = (msg_len < 16) ? msg_len : 16;
      std::memcpy(local_buf, msg_buf, copy_len);

      ocudu::fapi::slot_indication ind;
      if (deserialize_slot_indication(local_buf, copy_len, cfg_.numerology, ind)) {
        slot_notifier_->on_slot_indication(ind);
      }
      break;
    }

    case scf_msg_id::CRC_INDICATION:
      if (ind_notifier_) {
        dispatch_crc_indication(static_cast<const uint8_t*>(msg_buf), msg_len,
                                cfg_.numerology, *ind_notifier_);
      }
      break;

    case scf_msg_id::RX_DATA_INDICATION:
      if (ind_notifier_) {
        dispatch_rx_data_indication(static_cast<const uint8_t*>(msg_buf), msg_len,
                                    static_cast<const uint8_t*>(data_buf), data_len,
                                    cfg_.numerology, *ind_notifier_);
      }
      break;

    case scf_msg_id::UCI_INDICATION:
      if (ind_notifier_) {
        dispatch_uci_indication(static_cast<const uint8_t*>(msg_buf), msg_len,
                                cfg_.numerology, *ind_notifier_);
      }
      break;

    case scf_msg_id::RACH_INDICATION:
      if (ind_notifier_) {
        dispatch_rach_indication(static_cast<const uint8_t*>(msg_buf), msg_len,
                                 cfg_.numerology, *ind_notifier_);
      }
      break;

    case scf_msg_id::SRS_INDICATION:
      if (ind_notifier_) {
        dispatch_srs_indication(static_cast<const uint8_t*>(msg_buf), msg_len,
                                cfg_.numerology, *ind_notifier_);
      }
      break;

    case scf_msg_id::PARAM_RESPONSE:
      if (p5_notifier_ && msg_len >= 9) {
        auto* buf = static_cast<const uint8_t*>(msg_buf);
        ocudu::fapi::param_response rsp{};
        rsp.error_code = to_error_code(buf[8]);
        p5_notifier_->on_param_response(rsp);
      }
      break;

    case scf_msg_id::CONFIG_RESPONSE: {
      // Extract error_code from CONFIG_RESPONSE body (byte 0 after body header)
      if (msg_buf && msg_len >= 9) {
        auto* buf = static_cast<const uint8_t*>(msg_buf);
        uint8_t error_code = buf[8]; // after FAPI header(2) + body header(6)
        std::fprintf(stdout, "[aerial_bridge] CONFIG_RESPONSE: error_code=0x%02x %s\n",
                     error_code, error_code == 0 ? "(OK — cell configured)" : "(FAILED)");
        if (error_code != 0 && msg_len >= 10) {
          uint8_t num_invalid = buf[9];
          std::fprintf(stdout, "[aerial_bridge] CONFIG_RESPONSE: %u invalid/unsupported TLVs\n",
                       num_invalid);
        }
        if (p5_notifier_) {
          ocudu::fapi::config_response rsp{};
          rsp.error_code = to_error_code(error_code);
          p5_notifier_->on_config_response(rsp);
        }
      }
      break;
    }

    case scf_msg_id::STOP_INDICATION:
      if (p5_notifier_) {
        ocudu::fapi::stop_indication ind{};
        p5_notifier_->on_stop_indication(ind);
      }
      break;

    case scf_msg_id::ERROR_INDICATION:
      if (error_notifier_ && msg_len >= 14) {
        auto* buf = static_cast<const uint8_t*>(msg_buf);
        uint16_t sfn = 0;
        uint16_t slot = 0;
        std::memcpy(&sfn, buf + 8, 2);
        std::memcpy(&slot, buf + 10, 2);

        ocudu::fapi::error_indication ind{};
        ind.slot = ocudu::slot_point(cfg_.numerology, sfn, slot);
        ind.message_id = to_message_type(buf[12]);
        ind.error_code = to_error_code(buf[13]);
        error_notifier_->on_error_indication(ind);
      }
      break;

    default:
      std::fprintf(stderr, "[aerial_bridge] Unknown FAPI message type 0x%02x from cell %d\n",
                   msg_id, cell_id);
      break;
  }
}

} // namespace aerial_bridge
