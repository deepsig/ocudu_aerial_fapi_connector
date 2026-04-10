// SPDX-FileCopyrightText: Copyright (c) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
// Aerial FAPI Bridge — FAPI Adaptor Implementation

#include "aerial_bridge/aerial_fapi_adaptor.h"
#include "aerial_bridge/msg_translator.h"

extern "C" {
#include "nv_ipc.h"
}

#include "ocudu/fapi/p5/p5_messages.h"
#include "ocudu/fapi/p7/messages/tx_data_request.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace aerial_bridge {

// ═══════════════════════════════════════════════════════════════
// P5 Requests Gateway
// ═══════════════════════════════════════════════════════════════

aerial_p5_gateway::aerial_p5_gateway(nvipc_transport& transport)
  : transport_(transport)
{
}

void aerial_p5_gateway::send_param_request(const ocudu::fapi::param_request& msg)
{
  nv_ipc_msg_t ipc_msg;
  std::memset(&ipc_msg, 0, sizeof(ipc_msg));
  ipc_msg.msg_id  = scf_msg_id::PARAM_REQUEST;
  ipc_msg.cell_id = 0;
  // PARAM_REQUEST has no body beyond the header.
  ipc_msg.msg_len  = 8; // FAPI header (2) + body header (6)
  ipc_msg.data_len = 0;

  if (transport_.tx_allocate(ipc_msg) < 0) {
    std::fprintf(stderr, "[aerial_bridge] Failed to allocate TX buffer for PARAM_REQUEST\n");
    return;
  }

  // Write minimal FAPI header.
  auto* buf = static_cast<uint8_t*>(ipc_msg.msg_buf);
  buf[0] = 1;  // message_count = 1
  buf[1] = 0;  // handle_id = cell 0
  // Body header: type_id (2 bytes LE) + length (4 bytes LE).
  uint16_t type_id = scf_msg_id::PARAM_REQUEST;
  uint32_t length  = 0;
  std::memcpy(buf + 2, &type_id, 2);
  std::memcpy(buf + 4, &length, 4);

  transport_.tx_send(ipc_msg);
  std::fprintf(stdout, "[aerial_bridge] Sent PARAM_REQUEST\n");
}

/// Append a single SCF FAPI TLV (tag + length + value) to the buffer.
/// Value is padded to a 4-byte boundary as per SCF FAPI spec.
/// @return pointer past the written TLV bytes.
// NOTE: Aerial L1 is built WITHOUT SCF_FAPI_10_04, so TLV length is uint16_t (2 bytes).
static uint8_t* add_tlv(uint8_t* p, uint16_t tag, uint16_t len, const void* value)
{
  std::memcpy(p, &tag, 2);
  p += 2;
  std::memcpy(p, &len, 2);
  p += 2;
  // Write value, then pad to 4-byte boundary.
  std::memcpy(p, value, len);
  uint32_t padded_len = ((len + 3) / 4) * 4;
  if (padded_len > len) {
    std::memset(p + len, 0, padded_len - len);
  }
  p += padded_len;
  return p;
}

void aerial_p5_gateway::send_config_request(const ocudu::fapi::config_request& msg)
{
  // ── TLV tag constants (SCF FAPI 10.04 / scf_5g_fapi.h) ──
  constexpr uint16_t TLV_PHY_CELL_ID            = 0x100C;
  constexpr uint16_t TLV_FRAME_DUPLEX_TYPE      = 0x100D;
  constexpr uint16_t TLV_NUM_TX_ANT             = 0x1005;
  constexpr uint16_t TLV_NUM_RX_ANT             = 0x100A;
  constexpr uint16_t TLV_DL_FREQ                = 0x1002;
  constexpr uint16_t TLV_UL_FREQ                = 0x1007;
  constexpr uint16_t TLV_SCS_COMMON             = 0x1010;
  constexpr uint16_t TLV_SSB_PBCH_POWER         = 0x100E;
  constexpr uint16_t TLV_SSB_PERIOD             = 0x101F;
  constexpr uint16_t TLV_SSB_SUBCARRIER_OFFSET  = 0x1020;
  constexpr uint16_t TLV_SSB_MASK               = 0x1022;
  constexpr uint16_t TLV_SSB_OFFSET_POINT_A     = 0x101D;
  constexpr uint16_t TLV_PRACH_SEQ_LEN          = 0x1011;
  constexpr uint16_t TLV_PRACH_SUBC_SPACING     = 0x1012;
  constexpr uint16_t TLV_PRACH_CONFIG_INDEX     = 0x1029;
  constexpr uint16_t TLV_NUM_PRACH_FD_OCCASIONS = 0x1014;
  constexpr uint16_t TLV_PRACH_ROOT_SEQ_INDEX   = 0x1015;
  constexpr uint16_t TLV_PRACH_ZERO_CORR_CONF   = 0x1018;
  constexpr uint16_t TLV_SSB_PER_RACH           = 0x101B;
  constexpr uint16_t TLV_DL_BANDWIDTH           = 0x1001;
  constexpr uint16_t TLV_DL_GRID_SIZE           = 0x1004;
  constexpr uint16_t TLV_UL_BANDWIDTH           = 0x1006;
  constexpr uint16_t TLV_UL_GRID_SIZE           = 0x1009;
  constexpr uint16_t TLV_RESTRICTED_SET_CONFIG  = 0x1013;
  constexpr uint16_t TLV_NUM_ROOT_SEQ           = 0x1016;
  constexpr uint16_t TLV_K1                     = 0x1017;
  constexpr uint16_t TLV_MIB                    = 0x1021;
  constexpr uint16_t TLV_TDD_PERIOD             = 0x1026;
  constexpr uint16_t TLV_SLOT_CONFIG            = 0x1027;

  // ── Hardcoded values for n78 TDD 20 MHz, 30 kHz SCS, 2T2R ──
  // NOTE: In L1 standalone mode (test_type=1), sending full TLVs triggers
  // cell creation which fails at DBT storage reset. Sending minimal TLVs
  // lets L1 tick and process DL_TTI without creating a full cell.
  // Set AERIAL_BRIDGE_FULL_CONFIG=1 to enable full TLV set.
  const uint16_t phy_cell_id           = 1;
  const uint8_t  frame_duplex_type     = 1;       // TDD
  const uint16_t num_tx_ant            = 2;
  const uint16_t num_rx_ant            = 2;
  const uint32_t dl_freq               = 3600000;  // kHz (3.6 GHz)
  const uint32_t ul_freq               = 3600000;  // kHz (TDD: same)
  const uint8_t  scs_common            = 1;        // 30 kHz (mu=1)
  const uint32_t ssb_pbch_power        = 0;        // 0 dBm (in 0.001 dBm units — 0)
  const uint8_t  ssb_period            = 2;        // 20 ms
  const uint8_t  ssb_subcarrier_offset = 0;
  const uint32_t ssb_mask_0            = 1;        // SSB in first position
  const uint32_t ssb_mask_1            = 0;
  const uint16_t ssb_offset_point_a    = 0;
  const uint8_t  prach_seq_len         = 1;        // Long sequence
  const uint8_t  prach_subc_spacing    = 1;        // 30 kHz
  const uint8_t  prach_config_index    = 159;      // Standard config index for TDD FR1
  const uint8_t  num_prach_fd_occ      = 1;
  const uint16_t prach_root_seq_index  = 1;
  const uint8_t  prach_zero_corr_conf  = 0;
  const uint8_t  ssb_per_rach          = 0;        // one-to-one mapping
  const uint16_t dl_bandwidth          = 273;      // PRBs (100 MHz @ 30 kHz SCS)
  const uint16_t ul_bandwidth          = 273;
  const uint8_t  restricted_set_config = 0;        // unrestricted
  const uint8_t  num_root_seq          = 1;
  const uint16_t k1                    = 0;        // freq offset in PRBs
  const uint8_t  mib[4]               = {0,0,0,0}; // 3 bytes MIB + 1 pad
  const uint8_t  tdd_period            = 6;        // 5ms (encoding from nv_phy_utils.hpp)

  // ── Count TLVs and compute body size ──
  // 9 carrier + 2 grid + 7 SSB + 10 PRACH + 2 TDD = 30 TLVs
  constexpr unsigned num_tlvs = 30;
  constexpr size_t max_body = 512; // SLOT_CONFIG alone is 140 bytes + headers
  constexpr size_t header_size = sizeof(scf_header) + sizeof(scf_body_header); // 2 + 6 = 8

  nv_ipc_msg_t ipc_msg;
  std::memset(&ipc_msg, 0, sizeof(ipc_msg));
  ipc_msg.msg_id   = scf_msg_id::CONFIG_REQUEST;
  ipc_msg.cell_id  = 0;
  ipc_msg.msg_len  = header_size + max_body;
  ipc_msg.data_len = 0;

  if (transport_.tx_allocate(ipc_msg) < 0) {
    std::fprintf(stderr, "[aerial_bridge] Failed to allocate TX buffer for CONFIG_REQUEST\n");
    return;
  }

  auto* buf = static_cast<uint8_t*>(ipc_msg.msg_buf);

  // ── FAPI header ──
  auto* hdr = reinterpret_cast<scf_header*>(buf);
  hdr->message_count = 1;
  hdr->handle_id     = 0;

  // Skip body header for now — fill length at the end.
  uint8_t* body = buf + header_size;

  // num_tlvs
  body[0] = static_cast<uint8_t>(num_tlvs);
  uint8_t* p = body + 1;

  // ── Carrier / cell TLVs (9) ──
  p = add_tlv(p, TLV_DL_BANDWIDTH,       2, &dl_bandwidth);
  p = add_tlv(p, TLV_DL_FREQ,            4, &dl_freq);
  p = add_tlv(p, TLV_NUM_TX_ANT,         2, &num_tx_ant);
  p = add_tlv(p, TLV_UL_BANDWIDTH,       2, &ul_bandwidth);
  p = add_tlv(p, TLV_UL_FREQ,            4, &ul_freq);
  p = add_tlv(p, TLV_NUM_RX_ANT,         2, &num_rx_ant);
  // DL/UL grid size: 5 × uint16_t for numerologies mu 0-4
  {
    uint16_t dl_grid[5] = {0, 273, 0, 0, 0}; // mu=1 (30kHz) = 273 PRBs
    p = add_tlv(p, TLV_DL_GRID_SIZE, 10, dl_grid);
    uint16_t ul_grid[5] = {0, 273, 0, 0, 0};
    p = add_tlv(p, TLV_UL_GRID_SIZE, 10, ul_grid);
  }
  p = add_tlv(p, TLV_PHY_CELL_ID,        2, &phy_cell_id);
  p = add_tlv(p, TLV_FRAME_DUPLEX_TYPE,  1, &frame_duplex_type);
  p = add_tlv(p, TLV_SCS_COMMON,         1, &scs_common);

  // ── SSB TLVs (7) ──
  p = add_tlv(p, TLV_SSB_PBCH_POWER,        4, &ssb_pbch_power);
  p = add_tlv(p, TLV_SSB_OFFSET_POINT_A,    2, &ssb_offset_point_a);
  p = add_tlv(p, TLV_SSB_PERIOD,            1, &ssb_period);
  p = add_tlv(p, TLV_SSB_SUBCARRIER_OFFSET, 1, &ssb_subcarrier_offset);
  p = add_tlv(p, TLV_MIB,                   3, mib);
  p = add_tlv(p, TLV_SSB_MASK,              4, &ssb_mask_0);
  p = add_tlv(p, TLV_SSB_MASK,              4, &ssb_mask_1);

  // ── PRACH TLVs (10) ──
  // ORDER MATTERS: ROOT_SEQ_INDEX increments the per-FD-occasion index in L1.
  // Send general PRACH params first, then per-FD-occasion block.
  p = add_tlv(p, TLV_PRACH_SEQ_LEN,          1, &prach_seq_len);
  p = add_tlv(p, TLV_PRACH_SUBC_SPACING,     1, &prach_subc_spacing);
  p = add_tlv(p, TLV_RESTRICTED_SET_CONFIG,   1, &restricted_set_config);
  p = add_tlv(p, TLV_PRACH_CONFIG_INDEX,     1, &prach_config_index);
  p = add_tlv(p, TLV_NUM_PRACH_FD_OCCASIONS, 1, &num_prach_fd_occ);
  // Per-FD-occasion block (1 occasion):
  p = add_tlv(p, TLV_PRACH_ROOT_SEQ_INDEX,   2, &prach_root_seq_index); // triggers fd_index++
  p = add_tlv(p, TLV_NUM_ROOT_SEQ,           1, &num_root_seq);
  p = add_tlv(p, TLV_K1,                     2, &k1);
  p = add_tlv(p, TLV_PRACH_ZERO_CORR_CONF,   1, &prach_zero_corr_conf);
  p = add_tlv(p, TLV_SSB_PER_RACH,           1, &ssb_per_rach);

  // ── TDD TLVs (2) ──
  p = add_tlv(p, TLV_TDD_PERIOD,             1, &tdd_period);
  // SLOT_CONFIG: flat array of 10 slots × 14 symbols = 140 bytes (DDDSU×2)
  {
    uint8_t slot_cfg[140];
    for (int s = 0; s < 10; s++) {
      uint8_t* slot = &slot_cfg[s * 14];
      if (s == 3 || s == 8) {
        // Special slot: 10 DL + 2 guard + 2 UL
        std::memset(slot, 0, 10);
        slot[10] = 2; slot[11] = 2;
        slot[12] = 1; slot[13] = 1;
      } else if (s == 4 || s == 9) {
        std::memset(slot, 1, 14); // UL slot
      } else {
        std::memset(slot, 0, 14); // DL slot
      }
    }
    p = add_tlv(p, TLV_SLOT_CONFIG, 140, slot_cfg);
  }

  // ── Finalize body header ──
  uint32_t body_len = static_cast<uint32_t>(p - body);
  auto* body_hdr = reinterpret_cast<scf_body_header*>(buf + sizeof(scf_header));
  body_hdr->type_id = scf_msg_id::CONFIG_REQUEST;
  body_hdr->length  = body_len;

  ipc_msg.msg_len = static_cast<int>(header_size + body_len);

  // Debug: dump first 32 bytes of the message
  std::fprintf(stdout, "[aerial_bridge] CONFIG_REQUEST hex dump (first 40 bytes): ");
  for (int i = 0; i < 40 && i < ipc_msg.msg_len; ++i) {
    std::fprintf(stdout, "%02x ", buf[i]);
  }
  std::fprintf(stdout, "\n");

  transport_.tx_send(ipc_msg);
  std::fprintf(stdout, "[aerial_bridge] Sent CONFIG_REQUEST (%u TLVs, %u body bytes, msg_len=%d)\n",
               num_tlvs, body_len, ipc_msg.msg_len);
}

void aerial_p5_gateway::send_start_request(const ocudu::fapi::start_request& msg)
{
  nv_ipc_msg_t ipc_msg;
  std::memset(&ipc_msg, 0, sizeof(ipc_msg));
  ipc_msg.msg_id   = scf_msg_id::START_REQUEST;
  ipc_msg.cell_id  = 0;
  ipc_msg.msg_len  = 8;
  ipc_msg.data_len = 0;

  if (transport_.tx_allocate(ipc_msg) < 0) {
    std::fprintf(stderr, "[aerial_bridge] Failed to allocate TX buffer for START_REQUEST\n");
    return;
  }

  auto* buf = static_cast<uint8_t*>(ipc_msg.msg_buf);
  buf[0] = 1;
  buf[1] = 0;
  uint16_t type_id = scf_msg_id::START_REQUEST;
  uint32_t length  = 0;
  std::memcpy(buf + 2, &type_id, 2);
  std::memcpy(buf + 4, &length, 4);

  transport_.tx_send(ipc_msg);
  std::fprintf(stdout, "[aerial_bridge] Sent START_REQUEST\n");
}

void aerial_p5_gateway::send_stop_request(const ocudu::fapi::stop_request& /*msg*/)
{
  nv_ipc_msg_t ipc_msg;
  std::memset(&ipc_msg, 0, sizeof(ipc_msg));
  ipc_msg.msg_id   = scf_msg_id::STOP_REQUEST;
  ipc_msg.cell_id  = 0;
  ipc_msg.msg_len  = 8;
  ipc_msg.data_len = 0;

  if (transport_.tx_allocate(ipc_msg) < 0) {
    return;
  }

  auto* buf = static_cast<uint8_t*>(ipc_msg.msg_buf);
  buf[0] = 1;
  buf[1] = 0;
  uint16_t type_id = scf_msg_id::STOP_REQUEST;
  uint32_t length  = 0;
  std::memcpy(buf + 2, &type_id, 2);
  std::memcpy(buf + 4, &length, 4);

  transport_.tx_send(ipc_msg);
  std::fprintf(stdout, "[aerial_bridge] Sent STOP_REQUEST\n");
}

// ═══════════════════════════════════════════════════════════════
// P7 Requests Gateway
// ═══════════════════════════════════════════════════════════════

aerial_p7_gateway::aerial_p7_gateway(nvipc_transport& transport)
  : transport_(transport)
{
}

void aerial_p7_gateway::send_dl_tti_request(const ocudu::fapi::dl_tti_request& msg)
{
  nv_ipc_msg_t ipc_msg;
  std::memset(&ipc_msg, 0, sizeof(ipc_msg));
  ipc_msg.msg_id   = scf_msg_id::DL_TTI_REQUEST;
  ipc_msg.cell_id  = 0;
  ipc_msg.msg_len  = 15000; // Max CPU_MSG buffer size.
  ipc_msg.data_len = 0;

  if (transport_.tx_allocate(ipc_msg) < 0) {
    std::fprintf(stderr, "[aerial_bridge] Failed to allocate TX buffer for DL_TTI_REQUEST\n");
    return;
  }

  int bytes = serialize_dl_tti_request(msg, static_cast<uint8_t*>(ipc_msg.msg_buf),
                                       ipc_msg.msg_len, 0);
  if (bytes < 0) {
    transport_.tx_release(ipc_msg);
    std::fprintf(stderr, "[aerial_bridge] Failed to serialize DL_TTI_REQUEST\n");
    return;
  }
  ipc_msg.msg_len = bytes;
  transport_.tx_send(ipc_msg);
}

void aerial_p7_gateway::send_ul_tti_request(const ocudu::fapi::ul_tti_request& msg)
{
  nv_ipc_msg_t ipc_msg;
  std::memset(&ipc_msg, 0, sizeof(ipc_msg));
  ipc_msg.msg_id   = scf_msg_id::UL_TTI_REQUEST;
  ipc_msg.cell_id  = 0;
  ipc_msg.msg_len  = 15000;
  ipc_msg.data_len = 0;

  if (transport_.tx_allocate(ipc_msg) < 0) {
    std::fprintf(stderr, "[aerial_bridge] Failed to allocate TX buffer for UL_TTI_REQUEST\n");
    return;
  }

  int bytes = serialize_ul_tti_request(msg, static_cast<uint8_t*>(ipc_msg.msg_buf),
                                       ipc_msg.msg_len, 0);
  if (bytes < 0) {
    transport_.tx_release(ipc_msg);
    return;
  }
  ipc_msg.msg_len = bytes;
  transport_.tx_send(ipc_msg);
}

void aerial_p7_gateway::send_ul_dci_request(const ocudu::fapi::ul_dci_request& msg)
{
  nv_ipc_msg_t ipc_msg;
  std::memset(&ipc_msg, 0, sizeof(ipc_msg));
  ipc_msg.msg_id   = scf_msg_id::UL_DCI_REQUEST;
  ipc_msg.cell_id  = 0;
  ipc_msg.msg_len  = 15000;
  ipc_msg.data_len = 0;

  if (transport_.tx_allocate(ipc_msg) < 0) {
    return;
  }

  int bytes = serialize_ul_dci_request(msg, static_cast<uint8_t*>(ipc_msg.msg_buf),
                                       ipc_msg.msg_len, 0);
  if (bytes < 0) {
    transport_.tx_release(ipc_msg);
    return;
  }
  ipc_msg.msg_len = bytes;
  transport_.tx_send(ipc_msg);
}

void aerial_p7_gateway::send_tx_data_request(const ocudu::fapi::tx_data_request& msg)
{
  static int tx_data_count = 0;
  tx_data_count++;
  nv_ipc_msg_t ipc_msg;
  std::memset(&ipc_msg, 0, sizeof(ipc_msg));
  ipc_msg.msg_id    = scf_msg_id::TX_DATA_REQUEST;
  ipc_msg.cell_id   = 0;
  ipc_msg.msg_len   = 15000;
  ipc_msg.data_len  = 576000; // CPU_DATA pool buffer size.
  ipc_msg.data_pool = 1;      // NV_IPC_MEMPOOL_CPU_DATA

  if (transport_.tx_allocate(ipc_msg) < 0) {
    std::fprintf(stderr, "[aerial_bridge] Failed to allocate TX buffer for TX_DATA_REQUEST\n");
    return;
  }

  size_t data_written = 0;
  int bytes = serialize_tx_data_request(msg,
                                        static_cast<uint8_t*>(ipc_msg.msg_buf), ipc_msg.msg_len,
                                        static_cast<uint8_t*>(ipc_msg.data_buf), ipc_msg.data_len,
                                        0, data_written);
  if (bytes < 0) {
    transport_.tx_release(ipc_msg);
    return;
  }
  ipc_msg.msg_len  = bytes;
  ipc_msg.data_len = data_written;

  // Debug: dump first TX_DATA PDU bytes
  if (tx_data_count <= 3) {
    auto* b = static_cast<uint8_t*>(ipc_msg.msg_buf);
    std::fprintf(stdout, "[aerial_bridge] TX_DATA #%d hex (first 40 bytes): ", tx_data_count);
    for (int i = 0; i < 40 && i < bytes; ++i) {
      std::fprintf(stdout, "%02x ", b[i]);
    }
    std::fprintf(stdout, "\n");
    std::fprintf(stdout, "[aerial_bridge] TX_DATA msg_len=%d data_len=%zu num_pdus=%zu\n",
                 bytes, data_written, msg.pdus.size());
  }

  transport_.tx_send(ipc_msg);
}

// ═══════════════════════════════════════════════════════════════
// P7 Last Request Notifier
// ═══════════════════════════════════════════════════════════════

aerial_last_request_notifier::aerial_last_request_notifier(nvipc_transport& transport)
  : transport_(transport)
{
}

void aerial_last_request_notifier::on_last_message(ocudu::slot_point /*slot*/)
{
  // Signal L1 that all FAPI messages for this slot have been sent.
  transport_.tx_sem_post();
}

// ═══════════════════════════════════════════════════════════════
// P5 Sector Adaptor
// ═══════════════════════════════════════════════════════════════

aerial_p5_adaptor::aerial_p5_adaptor(nvipc_transport& transport, rx_thread& rx)
  : p5_gateway_(transport), rx_(rx)
{
}

ocudu::fapi::p5_requests_gateway& aerial_p5_adaptor::get_p5_requests_gateway()
{
  return p5_gateway_;
}

void aerial_p5_adaptor::set_p5_responses_notifier(ocudu::fapi::p5_responses_notifier& notifier)
{
  std::fprintf(stdout, "[aerial_bridge] P5 responses_notifier registered\n");
  rx_.set_p5_responses_notifier(&notifier);
}

void aerial_p5_adaptor::set_error_indication_notifier(ocudu::fapi::error_indication_notifier& notifier)
{
  rx_.set_error_notifier(&notifier);
}

// ═══════════════════════════════════════════════════════════════
// P7 Sector Adaptor
// ═══════════════════════════════════════════════════════════════

aerial_p7_adaptor::aerial_p7_adaptor(nvipc_transport& transport, rx_thread& rx)
  : p7_gateway_(transport), last_req_notifier_(transport), rx_(rx)
{
}

ocudu::fapi::p7_requests_gateway& aerial_p7_adaptor::get_p7_requests_gateway()
{
  return p7_gateway_;
}

ocudu::fapi::p7_last_request_notifier& aerial_p7_adaptor::get_p7_last_request_notifier()
{
  return last_req_notifier_;
}

void aerial_p7_adaptor::set_p7_slot_indication_notifier(
    ocudu::fapi::p7_slot_indication_notifier& notifier)
{
  std::fprintf(stdout, "[aerial_bridge] P7 slot_indication_notifier registered\n");
  rx_.set_slot_indication_notifier(&notifier);
}

void aerial_p7_adaptor::set_error_indication_notifier(
    ocudu::fapi::error_indication_notifier& notifier)
{
  rx_.set_error_notifier(&notifier);
}

void aerial_p7_adaptor::set_p7_indications_notifier(
    ocudu::fapi::p7_indications_notifier& notifier)
{
  std::fprintf(stdout, "[aerial_bridge] P7 indications_notifier registered\n");
  rx_.set_indications_notifier(&notifier);
}

// ═══════════════════════════════════════════════════════════════
// Sector Adaptor (combines P5 + P7)
// ═══════════════════════════════════════════════════════════════

aerial_sector_adaptor::aerial_sector_adaptor(nvipc_transport& transport, rx_thread& rx)
  : p5_(transport, rx), p7_(transport, rx)
{
}

ocudu::fapi_adaptor::phy_fapi_p5_sector_adaptor& aerial_sector_adaptor::get_p5_sector_adaptor()
{
  return p5_;
}

ocudu::fapi_adaptor::phy_fapi_p7_sector_adaptor& aerial_sector_adaptor::get_p7_sector_adaptor()
{
  return p7_;
}

void aerial_sector_adaptor::start()
{
  reset_bridge_runtime_metrics();
  std::fprintf(stdout, "[aerial_bridge] Sector adaptor starting — sending CONFIG_REQUEST...\n");

  // Send CONFIG_REQUEST to transition L1 from IDLE → CONFIGURED.
  ocudu::fapi::config_request cfg_req;
  p5_.get_p5_requests_gateway().send_config_request(cfg_req);

  // Give L1 time to process CONFIG and create GPU cell resources.
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));

  // Send START_REQUEST to transition L1 from CONFIGURED → RUNNING.
  ocudu::fapi::start_request start_req;
  p5_.get_p5_requests_gateway().send_start_request(start_req);
  std::fprintf(stdout, "[aerial_bridge] START_REQUEST sent — L1 should begin SLOT_INDs\n");
}

void aerial_sector_adaptor::stop()
{
  dump_bridge_runtime_metrics();
  std::fprintf(stdout, "[aerial_bridge] Sector adaptor stopped\n");
}

// ═══════════════════════════════════════════════════════════════
// Main FAPI Adaptor
// ═══════════════════════════════════════════════════════════════

aerial_fapi_adaptor::aerial_fapi_adaptor(const nvipc_transport_config& nvipc_cfg,
                                         const rx_thread_config&       rx_cfg,
                                         unsigned                      num_cells)
  : rx_(transport_)
{
  // Connect to Aerial L1 via nvIPC.
  if (!transport_.connect(nvipc_cfg)) {
    throw std::runtime_error("[aerial_bridge] Failed to connect to Aerial L1 via nvIPC "
                             "(prefix: " + nvipc_cfg.prefix + ")");
  }
  std::fprintf(stdout, "[aerial_bridge] Connected to Aerial L1 via nvIPC (prefix: %s)\n",
               nvipc_cfg.prefix.c_str());

  // Create sector adaptors.
  for (unsigned i = 0; i < num_cells; ++i) {
    sectors_.push_back(std::make_unique<aerial_sector_adaptor>(transport_, rx_));
  }

  // Start the RX polling thread.
  rx_.start(rx_cfg);
  std::fprintf(stdout, "[aerial_bridge] RX thread started (core=%d, prio=%d, numerology=%u)\n",
               rx_cfg.cpu_core, rx_cfg.priority, rx_cfg.numerology);
}

aerial_fapi_adaptor::~aerial_fapi_adaptor()
{
  rx_.stop();
  transport_.disconnect();
  std::fprintf(stdout, "[aerial_bridge] Disconnected from Aerial L1\n");
}

ocudu::fapi_adaptor::phy_fapi_sector_adaptor& aerial_fapi_adaptor::get_sector_adaptor(unsigned cell_id)
{
  if (cell_id >= sectors_.size()) {
    throw std::out_of_range("[aerial_bridge] Invalid cell_id " + std::to_string(cell_id));
  }
  return *sectors_[cell_id];
}

} // namespace aerial_bridge
