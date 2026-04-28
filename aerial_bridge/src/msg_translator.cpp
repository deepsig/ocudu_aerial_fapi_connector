// SPDX-FileCopyrightText: Copyright (c) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
// Aerial FAPI Bridge — Message Translator Implementation

#include "aerial_bridge/msg_translator.h"

#include "ocudu/fapi/p7/messages/dl_tti_request.h"
#include "ocudu/fapi/p7/messages/ul_tti_request.h"
#include "ocudu/fapi/p7/messages/ul_dci_request.h"
#include "ocudu/fapi/p7/messages/tx_data_request.h"
#include "ocudu/fapi/p7/messages/slot_indication.h"
#include "ocudu/fapi/p7/messages/crc_indication.h"
#include "ocudu/fapi/p7/messages/rx_data_indication.h"
#include "ocudu/fapi/p7/messages/rach_indication.h"
#include "ocudu/fapi/p7/messages/uci_indication.h"
#include "ocudu/fapi/p7/messages/srs_indication.h"
#include "ocudu/fapi/p7/p7_indications_notifier.h"
#include "ocudu/ran/phy_time_unit.h"
#include "ocudu/ran/slot_point.h"
#include "ocudu/ran/slot_point_extended.h"
#include "ocudu/ran/subcarrier_spacing.h"

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <limits>
#include <variant>

namespace aerial_bridge {

namespace {

#pragma pack(push, 1)

struct scf_uci_pdu {
  uint16_t pdu_type;
  uint16_t pdu_size;
  uint8_t  payload[0];
};

struct scf_uci_pusch_pdu {
  uint8_t  pdu_bitmap;
  uint32_t handle;
  uint16_t rnti;
  uint8_t  ul_cqi;
  uint16_t timing_advance;
  uint16_t rssi;
  uint8_t  payload[0];
};

struct scf_uci_pucch_pdu {
  uint8_t  pdu_bitmap;
  uint32_t handle;
  uint16_t rnti;
  uint8_t  pucch_format;
  uint8_t  ul_cqi;
  uint16_t timing_advance;
  uint16_t rssi;
  uint8_t  payload[0];
};

struct scf_uci_sr_f0_f1 {
  uint8_t sr_indication;
  uint8_t sr_confidence_level;
};

struct scf_uci_harq_f0_f1 {
  uint8_t num_harq;
  uint8_t harq_confidence_level;
  uint8_t harq_value[0];
};

struct scf_uci_sr_f2_f3_f4 {
  uint16_t sr_bit_len;
  uint8_t  sr_payload[0];
};

struct scf_uci_harq_f2_f3_f4 {
  uint8_t  harq_crc;
  uint16_t harq_bit_len;
  uint8_t  harq_payload[0];
};

struct scf_uci_csi_part {
  uint8_t  crc;
  uint16_t bit_len;
  uint8_t  payload[0];
};

struct scf_srs_ind_pdu_start {
  uint32_t handle;
  uint16_t rnti;
  uint16_t timing_advance;
  uint8_t  num_symbols;
  uint8_t  wide_band_snr;
  uint8_t  num_reported_symbols;
  uint8_t  payload[0];
};

struct scf_srs_ind_pdu_end {
  uint16_t num_rbs;
  uint8_t  rb_snr[0];
};

#pragma pack(pop)

struct pdsch_mcs_entry {
  uint8_t qam_mod_order;
  float target_code_rate;
};

static constexpr pdsch_mcs_entry pdsch_mcs_tables[4][32] = {
  {
    {2, 120}, {2, 157}, {2, 193}, {2, 251}, {2, 308}, {2, 379}, {2, 449}, {2, 526},
    {2, 602}, {2, 679}, {4, 340}, {4, 378}, {4, 434}, {4, 490}, {4, 553}, {4, 616},
    {4, 658}, {6, 438}, {6, 466}, {6, 517}, {6, 567}, {6, 616}, {6, 666}, {6, 719},
    {6, 772}, {6, 822}, {6, 873}, {6, 910}, {6, 948}, {2, 0},   {4, 0},   {6, 0},
  },
  {
    {2, 120}, {2, 193}, {2, 308}, {2, 449}, {2, 602}, {4, 378}, {4, 434}, {4, 490},
    {4, 553}, {4, 616}, {4, 658}, {6, 466}, {6, 517}, {6, 567}, {6, 616}, {6, 666},
    {6, 719}, {6, 772}, {6, 822}, {6, 873}, {8, 682.5f}, {8, 711}, {8, 754}, {8, 797},
    {8, 841}, {8, 885}, {8, 916.5f}, {8, 948}, {2, 0}, {4, 0}, {6, 0}, {8, 0},
  },
  {
    {2, 30},  {2, 40},  {2, 50},  {2, 64},  {2, 78},  {2, 99},  {2, 120}, {2, 157},
    {2, 193}, {2, 251}, {2, 308}, {2, 379}, {2, 449}, {2, 526}, {2, 602}, {4, 340},
    {4, 378}, {4, 434}, {4, 490}, {4, 553}, {4, 616}, {6, 438}, {6, 466}, {6, 517},
    {6, 567}, {6, 616}, {6, 666}, {6, 719}, {6, 772}, {2, 0},   {4, 0},   {6, 0},
  },
  {
    {2, 120}, {2, 193}, {2, 449}, {4, 378}, {4, 490}, {4, 616}, {6, 466}, {6, 517},
    {6, 567}, {6, 616}, {6, 666}, {6, 719}, {6, 772}, {6, 822}, {6, 873}, {8, 682.5f},
    {8, 711}, {8, 754}, {8, 797}, {8, 841}, {8, 885}, {8, 916.5f}, {8, 948}, {10, 805.5f},
    {10, 853}, {10, 900.5f}, {10, 948}, {2, 0}, {4, 0}, {6, 0}, {8, 0}, {10, 0},
  },
};

static pdsch_mcs_entry get_pdsch_mcs_entry(unsigned mcs_table, unsigned mcs_index)
{
  unsigned table = (mcs_table < 4) ? mcs_table : 0;
  unsigned index = (mcs_index < 32) ? mcs_index : 0;
  return pdsch_mcs_tables[table][index];
}

static uint8_t get_effective_pdsch_mcs_table(uint8_t requested_table)
{
  static const bool force_table0 = [] {
    const char* value = std::getenv("AERIAL_BRIDGE_FORCE_PDSCH_MCS_TABLE0");
    return value != nullptr && std::strcmp(value, "0") != 0;
  }();
  return force_table0 ? uint8_t{0} : requested_table;
}

static std::optional<ocudu::phy_time_unit> decode_timing_advance(uint16_t raw_ta, unsigned numerology)
{
  if (raw_ta == std::numeric_limits<uint16_t>::max()) {
    return std::nullopt;
  }

  return ocudu::phy_time_unit::from_timing_advance(raw_ta, ocudu::to_subcarrier_spacing(numerology));
}

static ocudu::uci_pusch_or_pucch_f2_3_4_detection_status decode_detection_status(uint8_t raw_status)
{
  switch (raw_status) {
    case 0:
      return ocudu::uci_pusch_or_pucch_f2_3_4_detection_status::crc_pass;
    case 1:
      return ocudu::uci_pusch_or_pucch_f2_3_4_detection_status::crc_failure;
    case 2:
      return ocudu::uci_pusch_or_pucch_f2_3_4_detection_status::dtx;
    case 3:
      return ocudu::uci_pusch_or_pucch_f2_3_4_detection_status::no_dtx;
    default:
      return ocudu::uci_pusch_or_pucch_f2_3_4_detection_status::dtx_not_checked;
  }
}

static bool log_uci_payload_bounds_error(const char* section,
                                         unsigned bit_len,
                                         unsigned max_bit_len,
                                         uint16_t sfn,
                                         uint16_t slot,
                                         uint16_t rnti)
{
  std::fprintf(stderr,
               "[aerial_bridge] Dropping malformed %s payload sfn=%u slot=%u rnti=0x%04x bits=%u max=%u\n",
               section,
               static_cast<unsigned>(sfn),
               static_cast<unsigned>(slot),
               static_cast<unsigned>(rnti),
               bit_len,
               max_bit_len);
  return false;
}

static std::optional<ocudu::uci_payload_type>
decode_payload_bits(const char* section, const uint8_t* payload, unsigned bit_len, uint16_t sfn, uint16_t slot, uint16_t rnti)
{
  if (bit_len > ocudu::uci_constants::MAX_NOF_PAYLOAD_BITS) {
    log_uci_payload_bounds_error(section, bit_len, ocudu::uci_constants::MAX_NOF_PAYLOAD_BITS, sfn, slot, rnti);
    return std::nullopt;
  }

  ocudu::uci_payload_type out(bit_len);
  for (unsigned bit = 0; bit != bit_len; ++bit) {
    const unsigned byte_index = bit / 8;
    const unsigned bit_index  = bit % 8;
    out.set(bit, ((payload[byte_index] >> bit_index) & 0x1U) != 0);
  }
  return out;
}

static std::optional<ocudu::bounded_bitset<ocudu::fapi::sr_pdu_format_2_3_4::MAX_SR_PAYLOAD_SIZE_BITS>>
decode_sr_payload_bits(const uint8_t* payload, unsigned bit_len, uint16_t sfn, uint16_t slot, uint16_t rnti)
{
  using sr_payload_t = ocudu::bounded_bitset<ocudu::fapi::sr_pdu_format_2_3_4::MAX_SR_PAYLOAD_SIZE_BITS>;
  if (bit_len > ocudu::fapi::sr_pdu_format_2_3_4::MAX_SR_PAYLOAD_SIZE_BITS) {
    log_uci_payload_bounds_error(
        "UCI SR", bit_len, ocudu::fapi::sr_pdu_format_2_3_4::MAX_SR_PAYLOAD_SIZE_BITS, sfn, slot, rnti);
    return std::nullopt;
  }

  sr_payload_t out(bit_len);
  for (unsigned bit = 0; bit != bit_len; ++bit) {
    const unsigned byte_index = bit / 8;
    const unsigned bit_index  = bit % 8;
    out.set(bit, ((payload[byte_index] >> bit_index) & 0x1U) != 0);
  }
  return out;
}

static bool should_log_indication(std::atomic<unsigned>& counter)
{
  const unsigned value = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  return value <= 16 || (value % 1000U) == 0U;
}

static std::atomic<unsigned> g_crc_dispatch_count{0};
static std::atomic<unsigned> g_uci_dispatch_count{0};
static std::atomic<unsigned> g_ul_pdu_serialize_log_count{0};
static std::atomic<unsigned> g_malformed_uci_log_count{0};

struct bridge_runtime_metrics {
  std::atomic<uint64_t> tx_data_reqs{0};
  std::atomic<uint64_t> tx_data_bytes{0};
  std::atomic<uint64_t> ul_pusch_pdus{0};
  std::atomic<uint64_t> ul_pucch_pdus{0};
  std::atomic<uint64_t> ul_prach_pdus{0};
  std::atomic<uint64_t> ul_srs_pdus{0};
  std::atomic<uint64_t> crc_total{0};
  std::atomic<uint64_t> crc_ok{0};
  std::atomic<uint64_t> crc_fail{0};
  std::atomic<uint64_t> rx_data_pdus{0};
  std::atomic<uint64_t> rx_data_bytes{0};
  std::atomic<uint64_t> uci_total{0};
  std::atomic<uint64_t> uci_pusch{0};
  std::atomic<uint64_t> uci_pucch01{0};
  std::atomic<uint64_t> uci_pucch234{0};
  std::atomic<uint64_t> rach_total{0};
  std::atomic<uint64_t> srs_total{0};
};

static bridge_runtime_metrics g_bridge_runtime_metrics;

static void reset_runtime_metric(std::atomic<uint64_t>& value)
{
  value.store(0, std::memory_order_relaxed);
}

static uint64_t load_runtime_metric(const std::atomic<uint64_t>& value)
{
  return value.load(std::memory_order_relaxed);
}

static bool should_log_limited(std::atomic<unsigned>& counter, unsigned limit)
{
  const unsigned value = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  return value <= limit;
}

static void format_hex_bytes(char* out, size_t out_len, const uint8_t* data, size_t data_len)
{
  if (out_len == 0) {
    return;
  }

  const size_t max_bytes = std::min<size_t>(data_len, 16);
  size_t written = 0;
  for (size_t i = 0; i != max_bytes && written + 3 < out_len; ++i) {
    const int n = std::snprintf(out + written, out_len - written, "%02x%s", data[i], (i + 1U == max_bytes) ? "" : " ");
    if (n <= 0) {
      break;
    }
    written += static_cast<size_t>(n);
  }
  if (max_bytes < data_len && written + 4 < out_len) {
    std::snprintf(out + written, out_len - written, " ...");
  }
}

static void log_outgoing_ul_pdu(const char* type,
                                unsigned sfn,
                                unsigned slot,
                                uint16_t rnti,
                                unsigned format_or_layers,
                                unsigned prb_start,
                                unsigned prb_size,
                                unsigned sym_start,
                                unsigned sym_len,
                                unsigned harq_id_or_count,
                                unsigned harq_bits,
                                unsigned sr_bits,
                                unsigned csi1_bits)
{
  if (!should_log_limited(g_ul_pdu_serialize_log_count, 64)) {
    return;
  }

  std::fprintf(stderr,
               "[aerial_bridge] UL serialize type=%s sfn=%u slot=%u rnti=0x%04x fmt/layers=%u prb=[%u,%u] sym=[%u,%u] harq_id/count=%u harq_bits=%u sr=%u csi1=%u\n",
               type,
               sfn,
               slot,
               static_cast<unsigned>(rnti),
               format_or_layers,
               prb_start,
               prb_size,
               sym_start,
               sym_len,
               harq_id_or_count,
               harq_bits,
               sr_bits,
               csi1_bits);
}

static void log_malformed_uci_bytes(const char* reason,
                                    const scf_uci_pdu& uci_pdu,
                                    const uint8_t* payload,
                                    size_t payload_len,
                                    uint16_t sfn,
                                    uint16_t slot)
{
  if (!should_log_limited(g_malformed_uci_log_count, 24)) {
    return;
  }

  char hex[3 * 16 + 8] = {};
  format_hex_bytes(hex, sizeof(hex), payload, payload_len);
  std::fprintf(stderr,
               "[aerial_bridge] Malformed UCI %s sfn=%u slot=%u pdu_type=%u pdu_size=%u payload_len=%zu raw=%s\n",
               reason,
               static_cast<unsigned>(sfn),
               static_cast<unsigned>(slot),
               static_cast<unsigned>(uci_pdu.pdu_type),
               static_cast<unsigned>(uci_pdu.pdu_size),
               payload_len,
               hex);
}

} // namespace

void reset_bridge_runtime_metrics()
{
  reset_runtime_metric(g_bridge_runtime_metrics.tx_data_reqs);
  reset_runtime_metric(g_bridge_runtime_metrics.tx_data_bytes);
  reset_runtime_metric(g_bridge_runtime_metrics.ul_pusch_pdus);
  reset_runtime_metric(g_bridge_runtime_metrics.ul_pucch_pdus);
  reset_runtime_metric(g_bridge_runtime_metrics.ul_prach_pdus);
  reset_runtime_metric(g_bridge_runtime_metrics.ul_srs_pdus);
  reset_runtime_metric(g_bridge_runtime_metrics.crc_total);
  reset_runtime_metric(g_bridge_runtime_metrics.crc_ok);
  reset_runtime_metric(g_bridge_runtime_metrics.crc_fail);
  reset_runtime_metric(g_bridge_runtime_metrics.rx_data_pdus);
  reset_runtime_metric(g_bridge_runtime_metrics.rx_data_bytes);
  reset_runtime_metric(g_bridge_runtime_metrics.uci_total);
  reset_runtime_metric(g_bridge_runtime_metrics.uci_pusch);
  reset_runtime_metric(g_bridge_runtime_metrics.uci_pucch01);
  reset_runtime_metric(g_bridge_runtime_metrics.uci_pucch234);
  reset_runtime_metric(g_bridge_runtime_metrics.rach_total);
  reset_runtime_metric(g_bridge_runtime_metrics.srs_total);
}

void dump_bridge_runtime_metrics()
{
  std::fprintf(stdout,
               "[aerial_bridge][summary] tx_data_reqs=%llu tx_data_bytes=%llu "
               "ul_pusch_pdus=%llu ul_pucch_pdus=%llu ul_prach_pdus=%llu ul_srs_pdus=%llu "
               "crc_total=%llu crc_ok=%llu crc_fail=%llu "
               "rx_data_pdus=%llu rx_data_bytes=%llu "
               "uci_total=%llu uci_pusch=%llu uci_pucch01=%llu uci_pucch234=%llu "
               "rach_total=%llu srs_total=%llu\n",
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.tx_data_reqs)),
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.tx_data_bytes)),
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.ul_pusch_pdus)),
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.ul_pucch_pdus)),
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.ul_prach_pdus)),
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.ul_srs_pdus)),
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.crc_total)),
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.crc_ok)),
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.crc_fail)),
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.rx_data_pdus)),
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.rx_data_bytes)),
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.uci_total)),
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.uci_pusch)),
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.uci_pucch01)),
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.uci_pucch234)),
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.rach_total)),
               static_cast<unsigned long long>(load_runtime_metric(g_bridge_runtime_metrics.srs_total)));
}

// ── Helper: write FAPI envelope + body header ───────────────

static size_t write_headers(uint8_t* buf, uint8_t cell_id, uint16_t type_id, uint32_t body_len)
{
  auto& hdr = *reinterpret_cast<scf_header*>(buf);
  hdr.message_count = 1;
  hdr.handle_id     = cell_id;

  auto& body = *reinterpret_cast<scf_body_header*>(buf + sizeof(scf_header));
  body.type_id = type_id;
  body.length  = body_len;

  return sizeof(scf_header) + sizeof(scf_body_header);
}

// ── Helper: extract SFN and slot from slot_point ────────────

static void slot_to_sfn_slot(const ocudu::slot_point& sp, uint16_t& sfn, uint16_t& slot)
{
  sfn  = sp.sfn();
  slot = sp.slot_index();
}

// ═══════════════════════════════════════════════════════════════
// DL_TTI.request serialization
// ═══════════════════════════════════════════════════════════════

/// Serialize a single SSB PDU into the buffer at the given offset.
static size_t serialize_ssb_pdu(const ocudu::fapi::dl_ssb_pdu& ssb, uint8_t* buf)
{
  auto& pdu_info = *reinterpret_cast<scf_generic_pdu_info*>(buf);
  pdu_info.pdu_type = scf_pdu_type::SSB;

  auto& ssb_pdu = *reinterpret_cast<scf_ssb_pdu*>(pdu_info.pdu_config);
  ssb_pdu.phys_cell_id         = ssb.phys_cell_id;
  ssb_pdu.ssb_block_index      = static_cast<uint8_t>(ssb.ssb_block_index);
  ssb_pdu.ssb_subcarrier_offset = static_cast<uint8_t>(ssb.subcarrier_offset.value());
  ssb_pdu.ssb_offset_pointA    = static_cast<uint16_t>(ssb.ssb_offset_pointA.value());
  ssb_pdu.bch_payload_flag     = 1; // PHY generates timing info.
  ssb_pdu.bch_payload          = ssb.bch_payload;

  // Power: use beta_pss = 0 (0 dB) for NR profile, or convert for SSS profile.
  if (const auto* nr = std::get_if<ocudu::fapi::dl_ssb_pdu::power_profile_nr>(&ssb.power_config)) {
    ssb_pdu.beta_pss = static_cast<uint8_t>(nr->beta_pss);
  } else {
    ssb_pdu.beta_pss = 0;
  }

  size_t pdu_data_size = sizeof(scf_ssb_pdu);
  pdu_info.pdu_size = sizeof(scf_generic_pdu_info) + pdu_data_size;
  return pdu_info.pdu_size;
}

/// Serialize a single PDCCH PDU into the buffer.
static size_t serialize_pdcch_pdu(const ocudu::fapi::dl_pdcch_pdu& pdcch, uint8_t* buf)
{
  auto& pdu_info = *reinterpret_cast<scf_generic_pdu_info*>(buf);
  pdu_info.pdu_type = scf_pdu_type::PDCCH;

  uint8_t* next = pdu_info.pdu_config;

  // BWP
  auto& bwp = *reinterpret_cast<scf_bwp*>(next);
  bwp.bwp_start     = pdcch.coreset_bwp.start();
  bwp.bwp_size      = pdcch.coreset_bwp.length();
  bwp.scs           = static_cast<uint8_t>(pdcch.scs);
  bwp.cyclic_prefix = pdcch.cp == ocudu::cyclic_prefix::NORMAL ? 0 : 1;
  next += sizeof(scf_bwp);

  // CORESET parameters
  *next++ = pdcch.symbols.start(); // start_sym_index
  *next++ = pdcch.symbols.length(); // duration_sym

  // Frequency domain resource bitmap (6 bytes / 45 bits)
  std::memset(next, 0, 6);
  for (unsigned i = 0; i < std::min<unsigned>(pdcch.freq_domain_resource.size(), 45); ++i) {
    if (pdcch.freq_domain_resource.test(i)) {
      next[i / 8] |= (1 << (i % 8));
    }
  }
  next += 6;

  // CCE-REG mapping type + parameters
  uint8_t cce_reg_type = 1; // non-interleaved
  uint8_t reg_bundle_sz = 6;
  uint8_t interleaver_sz = 0;
  uint8_t coreset_type = 0;
  uint16_t shift_index = 0;

  if (const auto* coreset0 = std::get_if<ocudu::fapi::dl_pdcch_pdu::mapping_coreset_0>(&pdcch.mapping)) {
    cce_reg_type = 0; // interleaved
    reg_bundle_sz = coreset0->interleaved.reg_bundle_sz;
    interleaver_sz = coreset0->interleaved.interleaver_sz;
    shift_index = coreset0->interleaved.shift_index;
    coreset_type = 1; // CORESET 0
  } else if (const auto* interleaved = std::get_if<ocudu::fapi::dl_pdcch_pdu::mapping_interleaved>(&pdcch.mapping)) {
    cce_reg_type = 0;
    reg_bundle_sz = interleaved->interleaved.reg_bundle_sz;
    interleaver_sz = interleaved->interleaved.interleaver_sz;
    shift_index = interleaved->interleaved.shift_index;
  } else if (const auto* non_interleaved = std::get_if<ocudu::fapi::dl_pdcch_pdu::mapping_non_interleaved>(&pdcch.mapping)) {
    cce_reg_type = 1;
    reg_bundle_sz = non_interleaved->reg_bundle_sz;
  }

  *next++ = cce_reg_type;
  *next++ = reg_bundle_sz;
  *next++ = interleaver_sz;
  *next++ = coreset_type;
  std::memcpy(next, &shift_index, 2); next += 2;
  *next++ = static_cast<uint8_t>(pdcch.precoder_granularity);

  // Number of DL DCIs = 1 (OCUDU sends one DCI per PDCCH PDU)
  uint16_t num_dl_dci = 1;
  std::memcpy(next, &num_dl_dci, 2); next += 2;

  // DCI payload
  const auto& dci = pdcch.dl_dci;
  auto& msg_dci = *reinterpret_cast<scf_dl_dci*>(next);
  uint16_t rnti = static_cast<uint16_t>(dci.rnti);
  msg_dci.rnti = rnti;
  msg_dci.scrambling_id = dci.nid_pdcch_dmrs;
  msg_dci.scrambling_rnti = dci.nrnti_pdcch_data;
  msg_dci.cce_index = dci.cce_index;
  msg_dci.aggregation_level = static_cast<uint8_t>(dci.dci_aggregation_level);
  next += sizeof(scf_dl_dci);

  // Aerial's parser expects a per-DCI beamforming block and tx-power info
  // ahead of the payload bits, even when no precoding/beamforming is used.
  auto& bf = *reinterpret_cast<scf_tx_precoding_beamforming*>(next);
  bf.num_prgs = 0;
  bf.prg_size = 0;
  bf.dig_bf_interfaces = 0;
  next += sizeof(scf_tx_precoding_beamforming);

  auto& pdcch_tx_power = *reinterpret_cast<scf_pdcch_tx_power_info*>(next);
  pdcch_tx_power.beta_pdcch_1_0 = 0;
  pdcch_tx_power.power_control_offset_ss = 1;
  next += sizeof(scf_pdcch_tx_power_info);

  auto& dci_payload = *reinterpret_cast<scf_pdcch_dci_payload*>(next);
  uint16_t payload_size = dci.payload.size();
  dci_payload.payload_size_bits = payload_size;
  next += sizeof(scf_pdcch_dci_payload);

  // Copy DCI payload bits (packed into bytes, MSB first)
  size_t payload_bytes = (payload_size + 7) / 8;
  std::memset(next, 0, payload_bytes);
  for (unsigned i = 0; i < payload_size; ++i) {
    if (dci.payload.test(i)) {
      next[i / 8] |= (1 << (7 - (i % 8)));
    }
  }
  next += payload_bytes;

  size_t total_size = next - buf;
  pdu_info.pdu_size = total_size;
  return total_size;
}

/// Serialize a single PDSCH PDU.
static size_t serialize_pdsch_pdu(const ocudu::fapi::dl_pdsch_pdu& pdsch, uint8_t* buf, uint16_t pdu_index)
{
  auto& pdu_info = *reinterpret_cast<scf_generic_pdu_info*>(buf);
  pdu_info.pdu_type = scf_pdu_type::PDSCH;

  uint8_t* next = pdu_info.pdu_config;

  // pdu_bitmap (bit 0 = PTRS, bit 1 = CBG retransmission)
  uint16_t pdu_bitmap = 0;
  std::memcpy(next, &pdu_bitmap, 2); next += 2;

  // RNTI (rnti_t is an enum class : uint16_t)
  uint16_t rnti = static_cast<uint16_t>(pdsch.rnti);
  std::memcpy(next, &rnti, 2); next += 2;

  // Keep the DL_TTI PDSCH index stable within the request so TX_DATA and
  // Aerial error accounting stay aligned when more than one PDSCH is present.
  std::memcpy(next, &pdu_index, 2); next += 2;

  // BWP (crb_interval has start() and length())
  auto& bwp = *reinterpret_cast<scf_bwp*>(next);
  bwp.bwp_start     = pdsch.bwp.start();
  bwp.bwp_size      = pdsch.bwp.length();
  bwp.scs           = static_cast<uint8_t>(pdsch.scs);
  bwp.cyclic_prefix = pdsch.cp == ocudu::cyclic_prefix::NORMAL ? 0 : 1;
  next += sizeof(scf_bwp);

  // Number of codewords
  uint8_t num_cw = pdsch.cws.size();
  *next++ = num_cw;

  // Codewords — must match scf_fapi_pdsch_codeword_t layout:
  //   target_code_rate(2) + qam_mod_order(1) + mcs_index(1) + mcs_table(1) + rv_index(1) + tb_size(4)
  for (const auto& cw : pdsch.cws) {
    const uint8_t effective_mcs_table = get_effective_pdsch_mcs_table(static_cast<uint8_t>(cw.mcs_table));
    auto mcs_entry = get_pdsch_mcs_entry(static_cast<unsigned>(effective_mcs_table),
                                         static_cast<unsigned>(static_cast<uint8_t>(cw.mcs_index)));
    uint16_t target_code_rate = static_cast<uint16_t>(mcs_entry.target_code_rate * 10.0f);
    std::memcpy(next, &target_code_rate, 2); next += 2;
    *next++ = mcs_entry.qam_mod_order != 0 ? mcs_entry.qam_mod_order : static_cast<uint8_t>(cw.qam_mod_order);
    *next++ = static_cast<uint8_t>(cw.mcs_index);
    *next++ = effective_mcs_table;
    *next++ = cw.rv_index;
    uint32_t tb_size = cw.tb_size.value();
    std::memcpy(next, &tb_size, 4); next += 4;
  }

  auto& end = *reinterpret_cast<scf_pdsch_pdu_end*>(next);
  end.data_scrambling_id = pdsch.nid_pdsch;
  end.num_of_layers = pdsch.num_layers;
  end.transmission_scheme = 0;
  end.ref_point = static_cast<uint8_t>(pdsch.ref_point);
  end.dl_dmrs_sym_pos = static_cast<uint16_t>(pdsch.dl_dmrs_symb_pos.to_uint64());
  end.dmrs_config_type = static_cast<uint8_t>(pdsch.dmrs_type);
  end.dl_dmrs_scrambling_id = pdsch.pdsch_dmrs_scrambling_id;
  end.sc_id = pdsch.nscid;
  end.num_dmrs_cdm_grps_no_data = pdsch.num_dmrs_cdm_grps_no_data;
  end.dmrs_ports = static_cast<uint16_t>(pdsch.dmrs_ports.to_uint64());
  end.resource_alloc = 1;
  std::memset(end.rb_bitmap, 0, sizeof(end.rb_bitmap));
  uint16_t rb_start = pdsch.resource_alloc.vrbs.start();
  end.rb_start = rb_start;
  uint16_t rb_size = pdsch.resource_alloc.vrbs.length();
  end.rb_size = rb_size;
  end.vrb_to_prb_mapping = static_cast<uint8_t>(pdsch.vrb_to_prb_mapping);
  end.start_sym_index = pdsch.symbols.start();
  end.num_symbols = pdsch.symbols.length();
  next += sizeof(scf_pdsch_pdu_end);

  // Aerial's parser unconditionally consumes a beamforming block and TX power info.
  // Emit the smallest valid "no beamforming" payload to keep the parser aligned.
  auto& bf = *reinterpret_cast<scf_tx_precoding_beamforming*>(next);
  bf.num_prgs = 0;
  bf.prg_size = 0;
  bf.dig_bf_interfaces = 0;
  next += sizeof(scf_tx_precoding_beamforming);

  auto& tx_power = *reinterpret_cast<scf_tx_power_info*>(next);
  tx_power.power_control_offset = 8;
  tx_power.power_control_offset_ss = 1;
  next += sizeof(scf_tx_power_info);

  size_t total_size = next - buf;
  pdu_info.pdu_size = total_size;

  // Debug: print PDSCH PDU key fields
  static unsigned pdsch_dbg_count = 0;
  if (pdsch_dbg_count < 5) {
    const uint8_t effective_mcs_table = get_effective_pdsch_mcs_table(static_cast<uint8_t>(pdsch.cws[0].mcs_table));
    auto dbg_mcs_entry = get_pdsch_mcs_entry(static_cast<unsigned>(effective_mcs_table),
                                             static_cast<unsigned>(static_cast<uint8_t>(pdsch.cws[0].mcs_index)));
    uint16_t dbg_target_code_rate = static_cast<uint16_t>(dbg_mcs_entry.target_code_rate * 10.0f);
    std::fprintf(stderr, "[bridge] PDSCH PDU: rnti=0x%04x pdu_idx=%u bwp=[%u,%u] scs=%u "
                 "num_cw=%u tb_size=%u Qm=%u mcs=%u mcs_table=%u tcr=%u rb=[%u,%u] sym=[%u,%u] layers=%u pdu_size=%u\n",
                 rnti, pdu_index, pdsch.bwp.start(), pdsch.bwp.length(),
                 static_cast<unsigned>(pdsch.scs), num_cw,
                 pdsch.cws[0].tb_size.value(),
                 static_cast<unsigned>(dbg_mcs_entry.qam_mod_order != 0 ? dbg_mcs_entry.qam_mod_order : static_cast<uint8_t>(pdsch.cws[0].qam_mod_order)),
                 static_cast<unsigned>(static_cast<uint8_t>(pdsch.cws[0].mcs_index)),
                 static_cast<unsigned>(effective_mcs_table),
                 dbg_target_code_rate,
                 rb_start, rb_size,
                 pdsch.symbols.start(), pdsch.symbols.length(),
                 pdsch.num_layers, static_cast<unsigned>(total_size));
    pdsch_dbg_count++;
  }

  return total_size;
}

int serialize_dl_tti_request(const ocudu::fapi::dl_tti_request& msg,
                             uint8_t* buf, size_t buf_len, uint8_t cell_id)
{
  if (buf_len < sizeof(scf_header) + sizeof(scf_body_header) + sizeof(scf_dl_tti_req)) {
    return -1;
  }

  // Reserve space for headers — fill body length later.
  size_t offset = sizeof(scf_header) + sizeof(scf_body_header);

  auto& req = *reinterpret_cast<scf_dl_tti_req*>(buf + offset);
  slot_to_sfn_slot(msg.slot, req.sfn, req.slot);
  req.num_pdus = 0;
  req.ngroup   = 0;

  size_t pdu_offset = offset + sizeof(scf_dl_tti_req);
  uint16_t next_pdsch_pdu_index = 0;

  for (const auto& pdu_wrapper : msg.pdus) {
    size_t pdu_size = 0;

    if (const auto* ssb = std::get_if<ocudu::fapi::dl_ssb_pdu>(&pdu_wrapper.pdu)) {
      pdu_size = serialize_ssb_pdu(*ssb, buf + pdu_offset);
    } else if (const auto* pdcch = std::get_if<ocudu::fapi::dl_pdcch_pdu>(&pdu_wrapper.pdu)) {
      pdu_size = serialize_pdcch_pdu(*pdcch, buf + pdu_offset);
    } else if (const auto* pdsch = std::get_if<ocudu::fapi::dl_pdsch_pdu>(&pdu_wrapper.pdu)) {
      pdu_size = serialize_pdsch_pdu(*pdsch, buf + pdu_offset, next_pdsch_pdu_index++);
    }
    // CSI-RS and PRS: TODO in later iteration.

    if (pdu_size > 0) {
      pdu_offset += pdu_size;
      req.num_pdus++;
    }
  }

  // Fill in headers.
  uint32_t body_len = pdu_offset - offset;
  write_headers(buf, cell_id, scf_msg_id::DL_TTI_REQUEST, body_len);

  return static_cast<int>(pdu_offset);
}

// ═══════════════════════════════════════════════════════════════
// UL_TTI.request serialization
// ═══════════════════════════════════════════════════════════════

/// Serialize a single PUSCH PDU into the buffer.
static size_t serialize_pusch_pdu(const ocudu::fapi::ul_pusch_pdu& pdu, uint8_t* buf)
{
  auto& pdu_info = *reinterpret_cast<scf_generic_pdu_info*>(buf);
  pdu_info.pdu_type = scf_pdu_type::PUSCH;

  uint8_t* next = pdu_info.pdu_config;

  // pdu_bitmap: bit 0 = pusch_data, bit 1 = pusch_uci, bit 2 = pusch_ptrs,
  //             bit 3 = dfts_ofdm
  uint16_t pdu_bitmap = 0;
  if (pdu.pusch_data.has_value()) {
    pdu_bitmap |= 0x01;
  }
  if (pdu.pusch_uci.has_value()) {
    pdu_bitmap |= 0x02;
  }
  if (std::holds_alternative<ocudu::fapi::ul_pusch_pdu::transform_precoding_enabled>(pdu.transform_precoding)) {
    pdu_bitmap |= 0x08;
  }
  std::memcpy(next, &pdu_bitmap, 2); next += 2;

  // RNTI
  uint16_t rnti = static_cast<uint16_t>(pdu.rnti);
  std::memcpy(next, &rnti, 2); next += 2;

  // Handle
  uint32_t handle = pdu.handle;
  std::memcpy(next, &handle, 4); next += 4;

  // BWP
  auto& bwp = *reinterpret_cast<scf_bwp*>(next);
  bwp.bwp_start     = pdu.bwp.start();
  bwp.bwp_size      = pdu.bwp.length();
  bwp.scs           = static_cast<uint8_t>(pdu.scs);
  bwp.cyclic_prefix = pdu.cp == ocudu::cyclic_prefix::NORMAL ? 0 : 1;
  next += sizeof(scf_bwp);

  // Target code rate (uint16_t in SCF, OCUDU uses float scaled by 10)
  uint16_t target_code_rate = static_cast<uint16_t>(pdu.target_code_rate * 10.0f);
  std::memcpy(next, &target_code_rate, 2); next += 2;

  // QAM modulation order
  *next++ = static_cast<uint8_t>(pdu.qam_mod_order);

  // MCS index
  *next++ = pdu.mcs_index;

  // MCS table
  *next++ = static_cast<uint8_t>(pdu.mcs_table);

  // Transform precoding flag and related fields
  uint8_t transform_precoding = 1;
  uint8_t num_dmrs_cdm_grps_no_data = 0;
  uint16_t dfts_ofdm_dmrs_identity = 0;
  if (std::get_if<ocudu::fapi::ul_pusch_pdu::transform_precoding_enabled>(&pdu.transform_precoding)) {
    transform_precoding = 0;
    const auto& tp = std::get<ocudu::fapi::ul_pusch_pdu::transform_precoding_enabled>(pdu.transform_precoding);
    dfts_ofdm_dmrs_identity = tp.pusch_dmrs_identity;
  } else if (const auto* tp_dis = std::get_if<ocudu::fapi::ul_pusch_pdu::transform_precoding_disabled>(&pdu.transform_precoding)) {
    transform_precoding = 1;
    num_dmrs_cdm_grps_no_data = tp_dis->num_dmrs_cdm_grps_no_data;
  }
  *next++ = transform_precoding;

  // nid_pusch (data scrambling identity)
  uint16_t nid_pusch = pdu.nid_pusch;
  std::memcpy(next, &nid_pusch, 2); next += 2;

  // Number of layers
  *next++ = pdu.num_layers;

  // UL DMRS symbol positions (bitmask)
  uint16_t ul_dmrs_symb_pos = static_cast<uint16_t>(pdu.ul_dmrs_symb_pos.to_uint64());
  std::memcpy(next, &ul_dmrs_symb_pos, 2); next += 2;

  // DMRS config type
  *next++ = static_cast<uint8_t>(pdu.dmrs_type);

  // PUSCH DMRS scrambling ID
  uint16_t pusch_dmrs_scrambling_id = pdu.pusch_dmrs_scrambling_id;
  std::memcpy(next, &pusch_dmrs_scrambling_id, 2); next += 2;

  // PUSCH identity (DFTS-OFDM DMRS identity)
  std::memcpy(next, &dfts_ofdm_dmrs_identity, 2); next += 2;

  // nSCID
  *next++ = pdu.nscid;

  // Number of DMRS CDM groups without data
  *next++ = num_dmrs_cdm_grps_no_data;

  // DMRS ports (bitmask)
  uint16_t dmrs_ports = static_cast<uint16_t>(pdu.dmrs_ports.to_uint64());
  std::memcpy(next, &dmrs_ports, 2); next += 2;

  // Resource allocation type (always type 1)
  *next++ = 1;

  // RB bitmap (36 bytes) — not used for type 1, zero fill.
  std::memset(next, 0, 36); next += 36;

  // RB start and size from resource_allocation_1
  uint16_t rb_start = pdu.resource_allocation_1.vrbs.start();
  std::memcpy(next, &rb_start, 2); next += 2;
  uint16_t rb_size = pdu.resource_allocation_1.vrbs.length();
  std::memcpy(next, &rb_size, 2); next += 2;

  // VRB-to-PRB mapping (0 = non-interleaved for UL)
  *next++ = 0;

  // Frequency hopping (0 = disabled)
  *next++ = 0;

  // TX direct current location
  uint16_t tx_dc_loc = pdu.tx_direct_current_location;
  std::memcpy(next, &tx_dc_loc, 2); next += 2;

  // UL frequency shift 7p5kHz (0 = disabled)
  *next++ = 0;

  // Time domain: start symbol and nr of symbols
  *next++ = pdu.symbols.start();
  *next++ = pdu.symbols.length();

  // ── Optional PUSCH data section (present when pdu_bitmap bit 0 set) ──
  if (pdu.pusch_data.has_value()) {
    const auto& data = pdu.pusch_data.value();

    *next++ = data.rv_index;
    *next++ = static_cast<uint8_t>(data.harq_process_id);
    *next++ = data.new_data ? 1 : 0;

    uint32_t tb_size = data.tb_size.value();
    std::memcpy(next, &tb_size, 4); next += 4;

    // Number of CB (0 = full TB)
    uint16_t num_cb = 0;
    std::memcpy(next, &num_cb, 2); next += 2;
    // cb_present_and_position bitmap omitted (num_cb = 0)
  }

  // ── Optional PUSCH UCI section (present when pdu_bitmap bit 1 set) ──
  if (pdu.pusch_uci.has_value()) {
    const auto& uci = pdu.pusch_uci.value();

    uint16_t harq_ack_bit = uci.harq_ack_bit.value();
    std::memcpy(next, &harq_ack_bit, 2); next += 2;

    uint16_t csi_part1_bit = uci.csi_part1_bit.value();
    std::memcpy(next, &csi_part1_bit, 2); next += 2;

    // CSI part 2 bit length (0 = not present)
    uint16_t csi_part2_bit = 0;
    std::memcpy(next, &csi_part2_bit, 2); next += 2;

    // Alpha scaling
    *next++ = static_cast<uint8_t>(uci.alpha_scaling);

    // Beta offsets
    *next++ = uci.beta_offset_harq_ack;
    *next++ = uci.beta_offset_csi1;
    *next++ = uci.beta_offset_csi2;
  }

  // DFT-s-OFDM fields are only present when transform precoding is enabled.
  if (transform_precoding == 0) {
    auto& dft = *reinterpret_cast<scf_pusch_dft_ofdm*>(next);
    dft.low_papr_group_number = 0;
    dft.low_papr_sequence_number = dfts_ofdm_dmrs_identity;
    dft.ul_ptrs_sample_density = 0;
    dft.ul_ptrs_time_density_transform_precoding = 0;
    next += sizeof(scf_pusch_dft_ofdm);
  }

  // Aerial always expects the uplink RX beamforming trailer after the optional PUSCH sections.
  auto& bf = *reinterpret_cast<scf_rx_beamforming*>(next);
  bf.num_prgs = 0;
  bf.prg_size = 0;
  bf.dig_bf_interfaces = 0;
  next += sizeof(scf_rx_beamforming);

  size_t total_size = next - buf;
  pdu_info.pdu_size = total_size;
  return total_size;
}

/// Serialize a single PUCCH PDU into the buffer.
static size_t serialize_pucch_pdu(const ocudu::fapi::ul_pucch_pdu& pdu, uint8_t* buf)
{
  auto& pdu_info = *reinterpret_cast<scf_generic_pdu_info*>(buf);
  pdu_info.pdu_type = scf_pdu_type::PUCCH;

  uint8_t* next = pdu_info.pdu_config;

  // RNTI
  uint16_t rnti = static_cast<uint16_t>(pdu.rnti);
  std::memcpy(next, &rnti, 2); next += 2;

  // Handle
  uint32_t handle = pdu.handle;
  std::memcpy(next, &handle, 4); next += 4;

  // BWP
  auto& bwp = *reinterpret_cast<scf_bwp*>(next);
  bwp.bwp_start     = pdu.bwp.start();
  bwp.bwp_size      = pdu.bwp.length();
  bwp.scs           = static_cast<uint8_t>(pdu.scs);
  bwp.cyclic_prefix = pdu.cp == ocudu::cyclic_prefix::NORMAL ? 0 : 1;
  next += sizeof(scf_bwp);

  // Determine PUCCH format from the variant
  uint8_t format_type = 0;
  if (std::get_if<ocudu::fapi::ul_pucch_pdu_format_0>(&pdu.format)) {
    format_type = 0;
  } else if (std::get_if<ocudu::fapi::ul_pucch_pdu_format_1>(&pdu.format)) {
    format_type = 1;
  } else if (std::get_if<ocudu::fapi::ul_pucch_pdu_format_2>(&pdu.format)) {
    format_type = 2;
  } else if (std::get_if<ocudu::fapi::ul_pucch_pdu_format_3>(&pdu.format)) {
    format_type = 3;
  } else if (std::get_if<ocudu::fapi::ul_pucch_pdu_format_4>(&pdu.format)) {
    format_type = 4;
  }
  *next++ = format_type;

  // multi_slot_tx_indicator (0 = no multi-slot)
  *next++ = 0;

  uint8_t pi2_bpsk = 0;
  if (const auto* f3 = std::get_if<ocudu::fapi::ul_pucch_pdu_format_3>(&pdu.format)) {
    pi2_bpsk = f3->pi2_bpsk ? 1U : 0U;
  } else if (const auto* f4 = std::get_if<ocudu::fapi::ul_pucch_pdu_format_4>(&pdu.format)) {
    pi2_bpsk = f4->pi2_bpsk ? 1U : 0U;
  }
  *next++ = pi2_bpsk;

  // PRB start and size
  uint16_t prb_start = pdu.prbs.start();
  std::memcpy(next, &prb_start, 2); next += 2;
  uint16_t prb_size = pdu.prbs.length();
  std::memcpy(next, &prb_size, 2); next += 2;

  // Start symbol index and nr of symbols
  *next++ = pdu.symbols.start();
  *next++ = pdu.symbols.length();

  // Intra-slot frequency hopping flag + second hop PRB
  uint8_t freq_hop = pdu.second_hop_prb.has_value() ? 1 : 0;
  *next++ = freq_hop;
  uint16_t second_hop = pdu.second_hop_prb.has_value() ? pdu.second_hop_prb.value() : 0;
  std::memcpy(next, &second_hop, 2); next += 2;

  uint8_t  group_hop_flag        = 0;
  uint8_t  seq_hop_flag          = 0;
  uint16_t hopping_id            = 0;
  uint16_t initial_cyclic_shift  = 0;
  uint16_t data_scrambling_id    = 0;
  uint8_t  time_domain_occ_idx   = 0;
  uint8_t  pre_dft_occ_idx       = 0;
  uint8_t  pre_dft_occ_len       = 0;
  uint8_t  add_dmrs_flag         = 0;
  uint16_t dmrs_scrambling_id    = 0;
  uint8_t  dmrs_cyclic_shift     = 0;
  uint8_t  sr_flag               = 0;
  uint16_t bit_len_harq          = 0;
  uint16_t bit_len_csi_part_1    = 0;
  uint16_t bit_len_csi_part_2    = 0;

  if (const auto* f0 = std::get_if<ocudu::fapi::ul_pucch_pdu_format_0>(&pdu.format)) {
    hopping_id           = f0->nid_pucch_hopping;
    initial_cyclic_shift = f0->initial_cyclic_shift;
    sr_flag              = f0->sr_present ? 1U : 0U;
    bit_len_harq         = static_cast<uint16_t>(f0->bit_len_harq.value());
  } else if (const auto* f1 = std::get_if<ocudu::fapi::ul_pucch_pdu_format_1>(&pdu.format)) {
    hopping_id           = f1->nid_pucch_hopping;
    initial_cyclic_shift = f1->initial_cyclic_shift;
    time_domain_occ_idx  = f1->time_domain_occ_index;
    sr_flag              = f1->sr_present ? 1U : 0U;
    bit_len_harq         = static_cast<uint16_t>(f1->bit_len_harq.value());
  } else if (const auto* f2 = std::get_if<ocudu::fapi::ul_pucch_pdu_format_2>(&pdu.format)) {
    data_scrambling_id = f2->nid_pucch_scrambling;
    dmrs_scrambling_id = f2->nid0_pucch_dmrs_scrambling;
    sr_flag            = static_cast<uint8_t>(f2->sr_bit_len);
    bit_len_harq       = static_cast<uint16_t>(f2->bit_len_harq.value());
    bit_len_csi_part_1 = static_cast<uint16_t>(f2->csi_part1_bit_length.value());
  } else if (const auto* f3 = std::get_if<ocudu::fapi::ul_pucch_pdu_format_3>(&pdu.format)) {
    hopping_id         = f3->nid_pucch_hopping;
    data_scrambling_id = f3->nid_pucch_scrambling;
    add_dmrs_flag      = f3->add_dmrs_flag ? 1U : 0U;
    dmrs_scrambling_id = f3->nid0_pucch_dmrs_scrambling;
    dmrs_cyclic_shift  = f3->m0_pucch_dmrs_cyclic_shift;
    sr_flag            = static_cast<uint8_t>(f3->sr_bit_len);
    bit_len_harq       = static_cast<uint16_t>(f3->bit_len_harq.value());
    bit_len_csi_part_1 = static_cast<uint16_t>(f3->csi_part1_bit_length.value());
  } else if (const auto* f4 = std::get_if<ocudu::fapi::ul_pucch_pdu_format_4>(&pdu.format)) {
    hopping_id         = f4->nid_pucch_hopping;
    data_scrambling_id = f4->nid_pucch_scrambling;
    pre_dft_occ_idx    = f4->pre_dft_occ_idx;
    pre_dft_occ_len    = f4->pre_dft_occ_len;
    add_dmrs_flag      = f4->add_dmrs_flag ? 1U : 0U;
    dmrs_scrambling_id = f4->nid0_pucch_dmrs_scrambling;
    dmrs_cyclic_shift  = f4->m0_pucch_dmrs_cyclic_shift;
    sr_flag            = static_cast<uint8_t>(f4->sr_bit_len);
    bit_len_harq       = static_cast<uint16_t>(f4->bit_len_harq.value());
    bit_len_csi_part_1 = static_cast<uint16_t>(f4->csi_part1_bit_length.value());
  }

  *next++ = group_hop_flag;
  *next++ = seq_hop_flag;
  std::memcpy(next, &hopping_id, 2); next += 2;
  std::memcpy(next, &initial_cyclic_shift, 2); next += 2;
  std::memcpy(next, &data_scrambling_id, 2); next += 2;
  *next++ = time_domain_occ_idx;
  *next++ = pre_dft_occ_idx;
  *next++ = pre_dft_occ_len;
  *next++ = add_dmrs_flag;
  std::memcpy(next, &dmrs_scrambling_id, 2); next += 2;
  *next++ = dmrs_cyclic_shift;
  *next++ = sr_flag;
  std::memcpy(next, &bit_len_harq, 2); next += 2;
  std::memcpy(next, &bit_len_csi_part_1, 2); next += 2;
  std::memcpy(next, &bit_len_csi_part_2, 2); next += 2;

  auto& bf = *reinterpret_cast<scf_rx_beamforming*>(next);
  bf.num_prgs = 0;
  bf.prg_size = 0;
  bf.dig_bf_interfaces = 0;
  next += sizeof(scf_rx_beamforming);

  size_t total_size = next - buf;
  pdu_info.pdu_size = total_size;
  return total_size;
}

/// Serialize a single SRS PDU into the buffer.
static size_t serialize_srs_pdu(const ocudu::fapi::ul_srs_pdu& pdu, uint8_t* buf)
{
  auto& pdu_info = *reinterpret_cast<scf_generic_pdu_info*>(buf);
  pdu_info.pdu_type = scf_pdu_type::SRS;

  uint8_t* next = pdu_info.pdu_config;

  // RNTI
  uint16_t rnti = static_cast<uint16_t>(pdu.rnti);
  std::memcpy(next, &rnti, 2); next += 2;

  // Handle
  uint32_t handle = pdu.handle;
  std::memcpy(next, &handle, 4); next += 4;

  // BWP
  auto& bwp = *reinterpret_cast<scf_bwp*>(next);
  bwp.bwp_start     = pdu.bwp.start();
  bwp.bwp_size      = pdu.bwp.length();
  bwp.scs           = static_cast<uint8_t>(pdu.scs);
  bwp.cyclic_prefix = pdu.cp == ocudu::cyclic_prefix::NORMAL ? 0 : 1;
  next += sizeof(scf_bwp);

  // Number of antenna ports
  *next++ = static_cast<uint8_t>(pdu.num_ant_ports);

  // Number of symbols
  *next++ = pdu.ofdm_symbols.start();
  *next++ = pdu.ofdm_symbols.length();

  // Number of repetitions
  *next++ = static_cast<uint8_t>(pdu.num_repetitions);

  // Time start position
  *next++ = pdu.time_start_position;

  // Config index
  *next++ = pdu.config_index;

  // Sequence ID
  uint16_t seq_id = pdu.sequence_id;
  std::memcpy(next, &seq_id, 2); next += 2;

  // Bandwidth index
  *next++ = pdu.bandwidth_index;

  // Comb size
  *next++ = static_cast<uint8_t>(pdu.comb_size);

  // Comb offset
  *next++ = pdu.comb_offset;

  // Cyclic shift
  *next++ = pdu.cyclic_shift;

  // Frequency position
  *next++ = pdu.frequency_position;

  // Frequency shift
  uint16_t freq_shift = pdu.frequency_shift;
  std::memcpy(next, &freq_shift, 2); next += 2;

  // Frequency hopping
  *next++ = pdu.frequency_hopping;

  // Group or sequence hopping
  *next++ = static_cast<uint8_t>(pdu.group_or_sequence_hopping);

  // Resource type
  *next++ = static_cast<uint8_t>(pdu.resource_type);

  // t_srs
  uint16_t t_srs = static_cast<uint16_t>(pdu.t_srs);
  std::memcpy(next, &t_srs, 2); next += 2;

  // t_offset
  uint16_t t_offset = pdu.t_offset;
  std::memcpy(next, &t_offset, 2); next += 2;

  size_t total_size = next - buf;
  pdu_info.pdu_size = total_size;
  return total_size;
}

int serialize_ul_tti_request(const ocudu::fapi::ul_tti_request& msg,
                             uint8_t* buf, size_t buf_len, uint8_t cell_id)
{
  if (buf_len < sizeof(scf_header) + sizeof(scf_body_header) + sizeof(scf_ul_tti_req)) {
    return -1;
  }

  size_t offset = sizeof(scf_header) + sizeof(scf_body_header);

  auto& req = *reinterpret_cast<scf_ul_tti_req*>(buf + offset);
  slot_to_sfn_slot(msg.slot, req.sfn, req.slot);
  req.num_pdus     = 0;
  req.rach_present = 0;
  req.num_ulsch    = 0;
  req.num_ulcch    = 0;
  req.ngroup       = 0;

  size_t pdu_offset = offset + sizeof(scf_ul_tti_req);

  for (const auto& pdu_wrapper : msg.pdus) {
    size_t pdu_size = 0;

    if (const auto* prach = std::get_if<ocudu::fapi::ul_prach_pdu>(&pdu_wrapper.pdu)) {
      auto& pdu_info = *reinterpret_cast<scf_generic_pdu_info*>(buf + pdu_offset);
      pdu_info.pdu_type = scf_pdu_type::PRACH;
      uint8_t* next = pdu_info.pdu_config;
      uint16_t pci = 0; // PCI set via cell config, not per-PDU in OCUDU.
      std::memcpy(next, &pci, 2); next += 2;
      *next++ = prach->num_prach_ocas;
      *next++ = static_cast<uint8_t>(prach->prach_format);
      *next++ = prach->index_fd_ra;
      *next++ = prach->prach_start_symbol;
      uint16_t num_cs = prach->num_cs;
      std::memcpy(next, &num_cs, 2); next += 2;
      pdu_info.pdu_size = next - reinterpret_cast<uint8_t*>(&pdu_info);
      pdu_size = pdu_info.pdu_size;
      req.rach_present = 1;
      g_bridge_runtime_metrics.ul_prach_pdus.fetch_add(1, std::memory_order_relaxed);
    } else if (const auto* pusch = std::get_if<ocudu::fapi::ul_pusch_pdu>(&pdu_wrapper.pdu)) {
      pdu_size = serialize_pusch_pdu(*pusch, buf + pdu_offset);
      req.num_ulsch++;
      g_bridge_runtime_metrics.ul_pusch_pdus.fetch_add(1, std::memory_order_relaxed);
      const unsigned harq_bits = pusch->pusch_uci.has_value() ? static_cast<unsigned>(pusch->pusch_uci->harq_ack_bit.value()) : 0U;
      const unsigned csi1_bits = pusch->pusch_uci.has_value() ? static_cast<unsigned>(pusch->pusch_uci->csi_part1_bit.value()) : 0U;
      log_outgoing_ul_pdu("PUSCH",
                          req.sfn,
                          req.slot,
                          static_cast<uint16_t>(pusch->rnti),
                          static_cast<unsigned>(pusch->num_layers),
                          pusch->resource_allocation_1.vrbs.start(),
                          pusch->resource_allocation_1.vrbs.length(),
                          pusch->symbols.start(),
                          pusch->symbols.length(),
                          pusch->pusch_data.has_value()
                              ? static_cast<unsigned>(pusch->pusch_data->harq_process_id)
                              : 0U,
                          harq_bits,
                          0U,
                          csi1_bits);
    } else if (const auto* pucch = std::get_if<ocudu::fapi::ul_pucch_pdu>(&pdu_wrapper.pdu)) {
      pdu_size = serialize_pucch_pdu(*pucch, buf + pdu_offset);
      req.num_ulcch++;
      g_bridge_runtime_metrics.ul_pucch_pdus.fetch_add(1, std::memory_order_relaxed);
      unsigned format_type = 0;
      unsigned harq_bits   = 0;
      unsigned sr_bits     = 0;
      unsigned csi1_bits   = 0;
      if (const auto* f0 = std::get_if<ocudu::fapi::ul_pucch_pdu_format_0>(&pucch->format)) {
        format_type = 0;
        harq_bits   = static_cast<unsigned>(f0->bit_len_harq.value());
        sr_bits     = f0->sr_present ? 1U : 0U;
      } else if (const auto* f1 = std::get_if<ocudu::fapi::ul_pucch_pdu_format_1>(&pucch->format)) {
        format_type = 1;
        harq_bits   = static_cast<unsigned>(f1->bit_len_harq.value());
        sr_bits     = f1->sr_present ? 1U : 0U;
      } else if (const auto* f2 = std::get_if<ocudu::fapi::ul_pucch_pdu_format_2>(&pucch->format)) {
        format_type = 2;
        harq_bits   = static_cast<unsigned>(f2->bit_len_harq.value());
        sr_bits     = static_cast<unsigned>(f2->sr_bit_len);
        csi1_bits   = static_cast<unsigned>(f2->csi_part1_bit_length.value());
      } else if (const auto* f3 = std::get_if<ocudu::fapi::ul_pucch_pdu_format_3>(&pucch->format)) {
        format_type = 3;
        harq_bits   = static_cast<unsigned>(f3->bit_len_harq.value());
        sr_bits     = static_cast<unsigned>(f3->sr_bit_len);
        csi1_bits   = static_cast<unsigned>(f3->csi_part1_bit_length.value());
      } else if (const auto* f4 = std::get_if<ocudu::fapi::ul_pucch_pdu_format_4>(&pucch->format)) {
        format_type = 4;
        harq_bits   = static_cast<unsigned>(f4->bit_len_harq.value());
        sr_bits     = static_cast<unsigned>(f4->sr_bit_len);
        csi1_bits   = static_cast<unsigned>(f4->csi_part1_bit_length.value());
      }
      log_outgoing_ul_pdu("PUCCH",
                          req.sfn,
                          req.slot,
                          static_cast<uint16_t>(pucch->rnti),
                          format_type,
                          pucch->prbs.start(),
                          pucch->prbs.length(),
                          pucch->symbols.start(),
                          pucch->symbols.length(),
                          harq_bits,
                          harq_bits,
                          sr_bits,
                          csi1_bits);
    } else if (const auto* srs = std::get_if<ocudu::fapi::ul_srs_pdu>(&pdu_wrapper.pdu)) {
      pdu_size = serialize_srs_pdu(*srs, buf + pdu_offset);
      g_bridge_runtime_metrics.ul_srs_pdus.fetch_add(1, std::memory_order_relaxed);
    }

    if (pdu_size > 0) {
      pdu_offset += pdu_size;
      req.num_pdus++;
    }
  }

  uint32_t body_len = pdu_offset - offset;
  write_headers(buf, cell_id, scf_msg_id::UL_TTI_REQUEST, body_len);

  return static_cast<int>(pdu_offset);
}

// ═══════════════════════════════════════════════════════════════
// UL_DCI.request serialization
// ═══════════════════════════════════════════════════════════════

int serialize_ul_dci_request(const ocudu::fapi::ul_dci_request& msg,
                             uint8_t* buf, size_t buf_len, uint8_t cell_id)
{
  if (buf_len < sizeof(scf_header) + sizeof(scf_body_header) + 8) {
    return -1;
  }

  size_t offset = sizeof(scf_header) + sizeof(scf_body_header);

  // UL_DCI uses the same format as DL_TTI for PDCCH PDUs.
  uint16_t sfn, slot;
  slot_to_sfn_slot(msg.slot, sfn, slot);

  auto* body = buf + offset;
  std::memcpy(body, &sfn, 2); body += 2;
  std::memcpy(body, &slot, 2); body += 2;
  uint8_t num_pdus = msg.pdus.size();
  *body++ = num_pdus;

  size_t pdu_start = body - buf;
  for (const auto& ul_dci : msg.pdus) {
    size_t pdu_size = serialize_pdcch_pdu(ul_dci.pdu, buf + pdu_start);
    // Override PDU type to PDCCH (UL DCI uses same struct).
    auto& pdu_info = *reinterpret_cast<scf_generic_pdu_info*>(buf + pdu_start);
    pdu_info.pdu_type = scf_pdu_type::PDCCH;
    pdu_start += pdu_size;
  }

  uint32_t body_len = pdu_start - offset;
  write_headers(buf, cell_id, scf_msg_id::UL_DCI_REQUEST, body_len);

  return static_cast<int>(pdu_start);
}

// ═══════════════════════════════════════════════════════════════
// TX_DATA.request serialization
// ═══════════════════════════════════════════════════════════════

int serialize_tx_data_request(const ocudu::fapi::tx_data_request& msg,
                              uint8_t* msg_buf, size_t msg_buf_len,
                              uint8_t* data_buf, size_t data_buf_len,
                              uint8_t cell_id,
                              size_t& data_bytes_written)
{
  data_bytes_written = 0;

  size_t offset = sizeof(scf_header) + sizeof(scf_body_header);
  if (msg_buf_len < offset + sizeof(scf_tx_data_req)) {
    return -1;
  }

  auto& req = *reinterpret_cast<scf_tx_data_req*>(msg_buf + offset);
  uint16_t sfn, slot;
  slot_to_sfn_slot(msg.slot, sfn, slot);
  req.sfn      = sfn;
  req.slot     = slot;
  req.num_pdus = msg.pdus.size();

  // Each TX_DATA PDU uses scf_fapi_tx_data_pdu_info_t format:
  // L1 built WITHOUT SCF_FAPI_10_04:
  //   pdu_len(4) + pdu_index(2) + num_tlv(4) + TLV(tag(2)+length(2)+value(4))
  // NO cw_index field. TLV length is uint16_t (2 bytes).
  constexpr uint16_t SCF_TX_DATA_OFFSET_TAG = 2;

  uint8_t* pdu_ptr = req.payload;
  size_t data_offset = 0;

  for (const auto& tx_pdu : msg.pdus) {
    auto tb_data = tx_pdu.pdu.get_buffer();
    uint32_t tb_len = tb_data.size();

    // pdu_len: L1 (without SCF_FAPI_10_04) uses pdu_len as TB data size
    uint32_t pdu_len = tb_len;
    std::memcpy(pdu_ptr, &pdu_len, 4); pdu_ptr += 4;

    // pdu_index
    uint16_t pdu_index = tx_pdu.pdu_index;
    std::memcpy(pdu_ptr, &pdu_index, 2); pdu_ptr += 2;

    // NOTE: cw_index field omitted — L1 standalone doesn't use it
    // despite SCF_FAPI_10_04 being defined in the header.

    // num_tlv = 1
    uint32_t num_tlv = 1;
    std::memcpy(pdu_ptr, &num_tlv, 4); pdu_ptr += 4;

    // TLV: tag = SCF_TX_DATA_OFFSET, length = tb_size, value = offset in data_buf
    // TLV length is uint16_t (L1 built WITHOUT SCF_FAPI_10_04)
    uint16_t tag = SCF_TX_DATA_OFFSET_TAG;
    std::memcpy(pdu_ptr, &tag, 2); pdu_ptr += 2;
    uint16_t tlv_length = static_cast<uint16_t>(tb_len);
    std::memcpy(pdu_ptr, &tlv_length, 2); pdu_ptr += 2;
    uint32_t offset_val = static_cast<uint32_t>(data_offset);
    std::memcpy(pdu_ptr, &offset_val, 4); pdu_ptr += 4;

    // Copy transport block data into nvIPC data buffer.
    if (data_buf && data_offset + tb_len <= data_buf_len) {
      std::memcpy(data_buf + data_offset, tb_data.data(), tb_len);
      // Pad to alignment (testMAC uses pdsch_align_bytes, default 32)
      uint32_t padding = (~tb_len + 1) & 31; // align to 32 bytes
      data_offset += tb_len + padding;
    }
  }

  data_bytes_written = data_offset;
  g_bridge_runtime_metrics.tx_data_reqs.fetch_add(1, std::memory_order_relaxed);
  g_bridge_runtime_metrics.tx_data_bytes.fetch_add(data_bytes_written, std::memory_order_relaxed);

  size_t total_msg = pdu_ptr - msg_buf;
  uint32_t body_len = total_msg - offset;
  write_headers(msg_buf, cell_id, scf_msg_id::TX_DATA_REQUEST, body_len);

  return static_cast<int>(total_msg);
}

// ═══════════════════════════════════════════════════════════════
// SLOT_INDICATION deserialization
// ═══════════════════════════════════════════════════════════════

bool deserialize_slot_indication(const uint8_t* buf, size_t buf_len,
                                 unsigned numerology,
                                 ocudu::fapi::slot_indication& ind)
{
  // Buffer contains: scf_header (2) + scf_body_header (6) + scf_slot_ind (4) = 12 bytes.
  if (buf_len < sizeof(scf_header) + sizeof(scf_body_header) + sizeof(scf_slot_ind)) {
    return false;
  }

  const auto& slot_ind = *reinterpret_cast<const scf_slot_ind*>(
      buf + sizeof(scf_header) + sizeof(scf_body_header));

  // Validate SFN (0-1023) and slot (0 to nof_slots_per_frame-1).
  uint16_t sfn  = slot_ind.sfn;
  uint16_t slot = slot_ind.slot;
  if (sfn > 1023) {
    return false;
  }

  // Infer numerology from the slot value if it exceeds our configured range.
  // L1 may use a different SCS than what OCUDU configured.
  unsigned effective_numerology = numerology;
  unsigned nof_slots_per_frame = 10u * (1u << effective_numerology);
  while (slot >= nof_slots_per_frame && effective_numerology < 4) {
    effective_numerology++;
    nof_slots_per_frame = 10u * (1u << effective_numerology);
  }
  if (slot >= nof_slots_per_frame) {
    return false;
  }

  ocudu::slot_point sp(effective_numerology, sfn, slot);
  ind.slot       = ocudu::slot_point_extended(sp);
  ind.time_point = std::chrono::system_clock::now();

  return true;
}

// ═══════════════════════════════════════════════════════════════
// CRC_INDICATION deserialization
// ═══════════════════════════════════════════════════════════════

// SCF CRC indication layout:
//   scf_header(2) + scf_body_header(6) + sfn(2) + slot(2) + num_crcs(2)
//   Per CRC:
//     non-10.04: handle(4) + rnti(2) + harq_id(1) + tb_crc_status(1) + num_cb(2) + cb_crc_status[0...]
//                 + end_info { ul_cqi(1) + timing_advance(2) + rssi(2) }
//     10.04:     handle(4) + rnti(2) + rapid(1) + harq_id(1) + tb_crc_status(1) + num_cb(2) + cb_crc_status[0...]
//                 + end_info { ul_sinr_metric(2) + timing_advance(2) + timing_advance_ns(2) + rssi(2) + rsrp(2) }

static constexpr size_t SCF_HDR_SIZE = sizeof(scf_header) + sizeof(scf_body_header);

void dispatch_crc_indication(const uint8_t* buf, size_t buf_len,
                             unsigned numerology,
                             ocudu::fapi::p7_indications_notifier& notifier)
{
  if (buf_len < SCF_HDR_SIZE + 6) return;

  const uint8_t* p = buf + SCF_HDR_SIZE;
  uint16_t sfn, slot, num_crcs;
  std::memcpy(&sfn, p, 2); p += 2;
  std::memcpy(&slot, p, 2); p += 2;
  std::memcpy(&num_crcs, p, 2); p += 2;

  ocudu::slot_point sp(numerology, sfn, slot);

  for (uint16_t i = 0; i < num_crcs; ++i) {
    if (p + 10 > buf + buf_len) break;

    ocudu::fapi::crc_indication ind;
    ind.slot = sp;

    uint32_t handle;
    std::memcpy(&handle, p, 4); p += 4;
    ind.pdu.handle = handle;

    uint16_t rnti;
    std::memcpy(&rnti, p, 2); p += 2;
    ind.pdu.rnti = static_cast<ocudu::rnti_t>(rnti);

    ind.pdu.harq_id = static_cast<ocudu::harq_id_t>(*p++);
    ind.pdu.tb_crc_status_ok = (*p++ == 0); // 0 = pass, 1 = fail

    uint16_t num_cb;
    std::memcpy(&num_cb, p, 2); p += 2;
    p += num_cb; // skip CB CRC status array

    // CRC end info (measurement fields) for the non-SCF_FAPI_10_04 layout.
    if (p + 5 <= buf + buf_len) {
      uint8_t ul_cqi = *p++;
      ind.pdu.ul_sinr_metric = ul_cqi;
      uint16_t ta;
      std::memcpy(&ta, p, 2); p += 2;
      ind.pdu.timing_advance_offset = std::nullopt;

      uint16_t rssi;
      std::memcpy(&rssi, p, 2); p += 2;
      ind.pdu.rssi = rssi;
      ind.pdu.rsrp = 0;
    }

    notifier.on_crc_indication(ind);
    g_bridge_runtime_metrics.crc_total.fetch_add(1, std::memory_order_relaxed);
    if (ind.pdu.tb_crc_status_ok) {
      g_bridge_runtime_metrics.crc_ok.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_bridge_runtime_metrics.crc_fail.fetch_add(1, std::memory_order_relaxed);
    }
    if (should_log_indication(g_crc_dispatch_count)) {
      std::fprintf(stderr,
                   "[aerial_bridge] CRC.indication #%u sfn=%u slot=%u rnti=0x%04x harq=%u ok=%u\n",
                   g_crc_dispatch_count.load(std::memory_order_relaxed),
                   sfn,
                   slot,
                   static_cast<unsigned>(rnti),
                   static_cast<unsigned>(ind.pdu.harq_id),
                   ind.pdu.tb_crc_status_ok ? 1U : 0U);
    }
  }
}

// ═══════════════════════════════════════════════════════════════
// RX_DATA_INDICATION deserialization
// ═══════════════════════════════════════════════════════════════

// SCF RX_DATA layout:
//   scf_header(2) + scf_body_header(6) + sfn(2) + slot(2)
//   non-10.04: num_pdus(2)
//   10.04:     control_length(2) + num_pdus(2)
//   Per PDU:
//   non-10.04: handle(4) + rnti(2) + harq_id(1) + pdu_len(4) + ul_cqi(1) + timing_advance(2) + rssi(2)
//   10.04:     handle(4) + rnti(2) + rapid(1) + harq_id(1) + pdu_len(4) + pdu_tag(1)

void dispatch_rx_data_indication(const uint8_t* buf, size_t buf_len,
                                 const uint8_t* data_buf, size_t data_len,
                                 unsigned numerology,
                                 ocudu::fapi::p7_indications_notifier& notifier)
{
  if (buf_len < SCF_HDR_SIZE + 6) return;

  const uint8_t* p = buf + SCF_HDR_SIZE;
  uint16_t sfn, slot;
  std::memcpy(&sfn, p, 2); p += 2;
  std::memcpy(&slot, p, 2); p += 2;

  uint16_t num_pdus;
  std::memcpy(&num_pdus, p, 2); p += 2;

  ocudu::slot_point sp(numerology, sfn, slot);
  size_t data_offset = 0;

  for (uint16_t i = 0; i < num_pdus; ++i) {
    if (p + 16 > buf + buf_len) break;

    ocudu::fapi::rx_data_indication ind;
    ind.slot = sp;

    uint32_t handle;
    std::memcpy(&handle, p, 4); p += 4;
    ind.pdu.handle = handle;

    uint16_t rnti;
    std::memcpy(&rnti, p, 2); p += 2;
    ind.pdu.rnti = static_cast<ocudu::rnti_t>(rnti);

    ind.pdu.harq_id = static_cast<ocudu::harq_id_t>(*p++);

    uint32_t pdu_len;
    std::memcpy(&pdu_len, p, 4); p += 4;
    p += 1; // skip ul_cqi
    p += 2; // skip timing_advance
    p += 2; // skip rssi

    // Transport block data is in data_buf at the current offset.
    if (data_buf && data_offset + pdu_len <= data_len) {
      ind.pdu.transport_block = ocudu::span<const uint8_t>(data_buf + data_offset, pdu_len);
      data_offset += pdu_len;
    } else {
      ind.pdu.transport_block = {};
    }

    notifier.on_rx_data_indication(ind);
    g_bridge_runtime_metrics.rx_data_pdus.fetch_add(1, std::memory_order_relaxed);
    g_bridge_runtime_metrics.rx_data_bytes.fetch_add(pdu_len, std::memory_order_relaxed);
  }
}

// ═══════════════════════════════════════════════════════════════
// RACH_INDICATION deserialization
// ═══════════════════════════════════════════════════════════════

// SCF RACH layout:
//   scf_header(2) + scf_body_header(6) + sfn(2) + slot(2) + num_pdus(1)
//   Per PDU: pci(2) + symbol_index(1) + slot_index(1) + freq_index(1)
//            + avg_rssi(1) + avg_snr(1) + num_preamble(1)
//   Per preamble: preamble_index(1) + timing_advance(2) + preamble_power(4)

void dispatch_rach_indication(const uint8_t* buf, size_t buf_len,
                              unsigned numerology,
                              ocudu::fapi::p7_indications_notifier& notifier)
{
  if (buf_len < SCF_HDR_SIZE + 5) return;

  const uint8_t* p = buf + SCF_HDR_SIZE;
  uint16_t sfn, slot;
  std::memcpy(&sfn, p, 2); p += 2;
  std::memcpy(&slot, p, 2); p += 2;

  uint8_t num_pdus = *p++;
  ocudu::slot_point sp(numerology, sfn, slot);

  for (uint8_t i = 0; i < num_pdus; ++i) {
    if (p + 8 > buf + buf_len) break;

    ocudu::fapi::rach_indication ind;
    ind.slot = sp;

    p += 2; // skip phys_cell_id (set via cell config)

    ind.pdu.symbol_index = *p++;
    ind.pdu.slot_index   = *p++;
    ind.pdu.ra_index     = *p++;
    ind.pdu.avg_rssi     = *p++;
    ind.pdu.avg_snr      = *p++;

    uint8_t num_preambles = *p++;

    for (uint8_t j = 0; j < num_preambles; ++j) {
      if (p + 7 > buf + buf_len) break;

      ocudu::fapi::rach_indication_pdu_preamble prmb;
      prmb.preamble_index = *p++;

      uint16_t ta;
      std::memcpy(&ta, p, 2); p += 2;
      // timing_advance_offset requires phy_time_unit conversion — skip for now.
      prmb.timing_advance_offset = std::nullopt;

      uint32_t pwr;
      std::memcpy(&pwr, p, 4); p += 4;
      prmb.preamble_pwr = pwr;

      prmb.preamble_snr = 0; // SCF struct doesn't include SNR per preamble.

      ind.pdu.preambles.push_back(prmb);
    }

    notifier.on_rach_indication(ind);
    g_bridge_runtime_metrics.rach_total.fetch_add(1, std::memory_order_relaxed);
  }
}

// ═══════════════════════════════════════════════════════════════
// UCI_INDICATION deserialization (stub — complex UCI parsing)
// ═══════════════════════════════════════════════════════════════

void dispatch_uci_indication(const uint8_t* buf, size_t buf_len,
                             unsigned numerology,
                             ocudu::fapi::p7_indications_notifier& notifier)
{
  if (buf_len < SCF_HDR_SIZE + 6) return;

  const uint8_t* p = buf + SCF_HDR_SIZE;
  uint16_t sfn, slot, num_ucis;
  std::memcpy(&sfn, p, 2); p += 2;
  std::memcpy(&slot, p, 2); p += 2;
  std::memcpy(&num_ucis, p, 2); p += 2;

  const ocudu::slot_point sp(numerology, sfn, slot);

  for (uint16_t i = 0; i != num_ucis; ++i) {
    if (p + sizeof(scf_uci_pdu) > buf + buf_len) {
      break;
    }

    const auto& uci_hdr = *reinterpret_cast<const scf_uci_pdu*>(p);
    if (uci_hdr.pdu_size < sizeof(scf_uci_pdu) || p + uci_hdr.pdu_size > buf + buf_len) {
      break;
    }

    const uint8_t* q     = uci_hdr.payload;
    const uint8_t* p_end = p + uci_hdr.pdu_size;

    ocudu::fapi::uci_indication ind;
    ind.slot = sp;

    switch (uci_hdr.pdu_type) {
      case 0: {
        if (q + sizeof(scf_uci_pusch_pdu) > p_end) {
          p = p_end;
          continue;
        }

        const auto& pusch_hdr = *reinterpret_cast<const scf_uci_pusch_pdu*>(q);
        q += sizeof(scf_uci_pusch_pdu);

        auto& uci = ind.pdu.emplace<ocudu::fapi::uci_pusch_pdu>();
        uci.handle                = pusch_hdr.handle;
        uci.rnti                  = static_cast<ocudu::rnti_t>(pusch_hdr.rnti);
        uci.ul_sinr_metric        = std::numeric_limits<int16_t>::min();
        uci.timing_advance_offset = decode_timing_advance(pusch_hdr.timing_advance, numerology);
        uci.rssi                  = pusch_hdr.rssi;
        uci.rsrp                  = std::numeric_limits<uint16_t>::max();

        if ((pusch_hdr.pdu_bitmap & 0x2U) != 0U && q + sizeof(scf_uci_harq_f2_f3_f4) <= p_end) {
          const auto& harq_hdr = *reinterpret_cast<const scf_uci_harq_f2_f3_f4*>(q);
          const unsigned num_bytes = (harq_hdr.harq_bit_len + 7U) / 8U;
          if (q + sizeof(scf_uci_harq_f2_f3_f4) + num_bytes <= p_end) {
            if (auto payload =
                    decode_payload_bits("UCI HARQ", harq_hdr.harq_payload, harq_hdr.harq_bit_len, sfn, slot, pusch_hdr.rnti)) {
              uci.harq = ocudu::fapi::uci_harq_pdu{
                  .detection_status    = decode_detection_status(harq_hdr.harq_crc),
                  .expected_bit_length = ocudu::units::bits(harq_hdr.harq_bit_len),
                  .payload             = std::move(*payload)};
            }
          }
          q += sizeof(scf_uci_harq_f2_f3_f4) + num_bytes;
        }

        if ((pusch_hdr.pdu_bitmap & 0x4U) != 0U && q + sizeof(scf_uci_csi_part) <= p_end) {
          const auto& csi1_hdr = *reinterpret_cast<const scf_uci_csi_part*>(q);
          const unsigned num_bytes = (csi1_hdr.bit_len + 7U) / 8U;
          if (q + sizeof(scf_uci_csi_part) + num_bytes <= p_end) {
            if (auto payload =
                    decode_payload_bits("UCI CSI-Part1", csi1_hdr.payload, csi1_hdr.bit_len, sfn, slot, pusch_hdr.rnti)) {
              uci.csi_part1 = ocudu::fapi::uci_csi_part1{
                  .detection_status    = decode_detection_status(csi1_hdr.crc),
                  .expected_bit_length = ocudu::units::bits(csi1_hdr.bit_len),
                  .payload             = std::move(*payload)};
            }
          }
          q += sizeof(scf_uci_csi_part) + num_bytes;
        }

        if ((pusch_hdr.pdu_bitmap & 0x8U) != 0U && q + sizeof(scf_uci_csi_part) <= p_end) {
          const auto& csi2_hdr = *reinterpret_cast<const scf_uci_csi_part*>(q);
          const unsigned num_bytes = (csi2_hdr.bit_len + 7U) / 8U;
          if (q + sizeof(scf_uci_csi_part) + num_bytes <= p_end) {
            if (auto payload =
                    decode_payload_bits("UCI CSI-Part2", csi2_hdr.payload, csi2_hdr.bit_len, sfn, slot, pusch_hdr.rnti)) {
              uci.csi_part2 = ocudu::fapi::uci_csi_part2{
                  .detection_status    = decode_detection_status(csi2_hdr.crc),
                  .expected_bit_length = ocudu::units::bits(csi2_hdr.bit_len),
                  .payload             = std::move(*payload)};
            }
          }
        }
        notifier.on_uci_indication(ind);
        g_bridge_runtime_metrics.uci_total.fetch_add(1, std::memory_order_relaxed);
        g_bridge_runtime_metrics.uci_pusch.fetch_add(1, std::memory_order_relaxed);
        if (should_log_indication(g_uci_dispatch_count)) {
          std::fprintf(stderr,
                       "[aerial_bridge] UCI.indication #%u sfn=%u slot=%u type=PUSCH rnti=0x%04x harq=%u csi1=%u csi2=%u\n",
                       g_uci_dispatch_count.load(std::memory_order_relaxed),
                       sfn,
                       slot,
                       static_cast<unsigned>(pusch_hdr.rnti),
                       uci.harq.has_value() ? 1U : 0U,
                       uci.csi_part1.has_value() ? 1U : 0U,
                       uci.csi_part2.has_value() ? 1U : 0U);
        }
        break;
      }

      case 1: {
        if (q + sizeof(scf_uci_pucch_pdu) > p_end) {
          p = p_end;
          continue;
        }

        const auto& pucch_hdr = *reinterpret_cast<const scf_uci_pucch_pdu*>(q);
        q += sizeof(scf_uci_pucch_pdu);

        auto& uci = ind.pdu.emplace<ocudu::fapi::uci_pucch_pdu_format_0_1>();
        uci.handle                = pucch_hdr.handle;
        uci.rnti                  = static_cast<ocudu::rnti_t>(pucch_hdr.rnti);
        uci.pucch_format          = (pucch_hdr.pucch_format == 0)
                                        ? ocudu::fapi::uci_pucch_pdu_format_0_1::format_type::format_0
                                        : ocudu::fapi::uci_pucch_pdu_format_0_1::format_type::format_1;
        uci.ul_sinr_metric        = std::numeric_limits<int16_t>::min();
        uci.timing_advance_offset = decode_timing_advance(pucch_hdr.timing_advance, numerology);
        uci.rssi                  = pucch_hdr.rssi;
        uci.rsrp                  = std::numeric_limits<uint16_t>::max();

        if ((pucch_hdr.pdu_bitmap & 0x1U) != 0U && q + sizeof(scf_uci_sr_f0_f1) <= p_end) {
          const auto& sr_hdr = *reinterpret_cast<const scf_uci_sr_f0_f1*>(q);
          uci.sr = ocudu::fapi::sr_pdu_format_0_1{.sr_detected = sr_hdr.sr_indication != 0U};
          q += sizeof(scf_uci_sr_f0_f1);
        }

        if ((pucch_hdr.pdu_bitmap & 0x2U) != 0U && q + sizeof(scf_uci_harq_f0_f1) <= p_end) {
          const auto& harq_hdr = *reinterpret_cast<const scf_uci_harq_f0_f1*>(q);
          if (q + sizeof(scf_uci_harq_f0_f1) + harq_hdr.num_harq <= p_end) {
            if (harq_hdr.num_harq <= ocudu::fapi::uci_harq_format_0_1::MAX_NUM_HARQ) {
              ocudu::fapi::uci_harq_format_0_1 harq;
              for (uint8_t n = 0; n != harq_hdr.num_harq; ++n) {
                harq.harq_values.push_back(static_cast<ocudu::uci_pucch_f0_or_f1_harq_values>(harq_hdr.harq_value[n]));
              }
              uci.harq = std::move(harq);
            } else {
              log_malformed_uci_bytes("PUCCH01-HARQ", uci_hdr, q, static_cast<size_t>(p_end - q), sfn, slot);
              std::fprintf(stderr,
                           "[aerial_bridge] Dropping malformed UCI HARQ-F0F1 sfn=%u slot=%u rnti=0x%04x harq=%u max=%u\n",
                           static_cast<unsigned>(sfn),
                           static_cast<unsigned>(slot),
                           static_cast<unsigned>(pucch_hdr.rnti),
                           static_cast<unsigned>(harq_hdr.num_harq),
                           ocudu::fapi::uci_harq_format_0_1::MAX_NUM_HARQ);
            }
          }
          q += sizeof(scf_uci_harq_f0_f1) + harq_hdr.num_harq;
        }
        notifier.on_uci_indication(ind);
        g_bridge_runtime_metrics.uci_total.fetch_add(1, std::memory_order_relaxed);
        g_bridge_runtime_metrics.uci_pucch01.fetch_add(1, std::memory_order_relaxed);
        if (should_log_indication(g_uci_dispatch_count)) {
          std::fprintf(stderr,
                       "[aerial_bridge] UCI.indication #%u sfn=%u slot=%u type=PUCCH01 fmt=%u rnti=0x%04x sr=%u harq_bits=%u\n",
                       g_uci_dispatch_count.load(std::memory_order_relaxed),
                       sfn,
                       slot,
                       static_cast<unsigned>(pucch_hdr.pucch_format),
                       static_cast<unsigned>(pucch_hdr.rnti),
                       uci.sr.has_value() && uci.sr->sr_detected ? 1U : 0U,
                       uci.harq.has_value() ? static_cast<unsigned>(uci.harq->harq_values.size()) : 0U);
        }
        break;
      }

      case 2: {
        if (q + sizeof(scf_uci_pucch_pdu) > p_end) {
          p = p_end;
          continue;
        }

        const auto& pucch_hdr = *reinterpret_cast<const scf_uci_pucch_pdu*>(q);
        q += sizeof(scf_uci_pucch_pdu);

        auto& uci = ind.pdu.emplace<ocudu::fapi::uci_pucch_pdu_format_2_3_4>();
        uci.handle                = pucch_hdr.handle;
        uci.rnti                  = static_cast<ocudu::rnti_t>(pucch_hdr.rnti);
        switch (pucch_hdr.pucch_format) {
          case 0:
            uci.pucch_format = ocudu::fapi::uci_pucch_pdu_format_2_3_4::format_type::format_2;
            break;
          case 1:
            uci.pucch_format = ocudu::fapi::uci_pucch_pdu_format_2_3_4::format_type::format_3;
            break;
          default:
            uci.pucch_format = ocudu::fapi::uci_pucch_pdu_format_2_3_4::format_type::format_4;
            break;
        }
        uci.ul_sinr_metric        = std::numeric_limits<int16_t>::min();
        uci.timing_advance_offset = decode_timing_advance(pucch_hdr.timing_advance, numerology);
        uci.rssi                  = pucch_hdr.rssi;
        uci.rsrp                  = std::numeric_limits<uint16_t>::max();

        if ((pucch_hdr.pdu_bitmap & 0x1U) != 0U && q + sizeof(scf_uci_sr_f2_f3_f4) <= p_end) {
          const auto& sr_hdr = *reinterpret_cast<const scf_uci_sr_f2_f3_f4*>(q);
          const unsigned num_bytes = (sr_hdr.sr_bit_len + 7U) / 8U;
          if (q + sizeof(scf_uci_sr_f2_f3_f4) + num_bytes <= p_end) {
            if (auto sr_payload = decode_sr_payload_bits(sr_hdr.sr_payload, sr_hdr.sr_bit_len, sfn, slot, pucch_hdr.rnti)) {
              uci.sr = ocudu::fapi::sr_pdu_format_2_3_4{.sr_payload = std::move(*sr_payload)};
            } else {
              log_malformed_uci_bytes("PUCCH234-SR", uci_hdr, q, static_cast<size_t>(p_end - q), sfn, slot);
            }
          }
          q += sizeof(scf_uci_sr_f2_f3_f4) + num_bytes;
        }

        if ((pucch_hdr.pdu_bitmap & 0x2U) != 0U && q + sizeof(scf_uci_harq_f2_f3_f4) <= p_end) {
          const auto& harq_hdr = *reinterpret_cast<const scf_uci_harq_f2_f3_f4*>(q);
          const unsigned num_bytes = (harq_hdr.harq_bit_len + 7U) / 8U;
          if (q + sizeof(scf_uci_harq_f2_f3_f4) + num_bytes <= p_end) {
            if (auto payload =
                    decode_payload_bits("UCI HARQ", harq_hdr.harq_payload, harq_hdr.harq_bit_len, sfn, slot, pucch_hdr.rnti)) {
              uci.harq = ocudu::fapi::uci_harq_pdu{
                  .detection_status    = decode_detection_status(harq_hdr.harq_crc),
                  .expected_bit_length = ocudu::units::bits(harq_hdr.harq_bit_len),
                  .payload             = std::move(*payload)};
            } else {
              log_malformed_uci_bytes("PUCCH234-HARQ", uci_hdr, q, static_cast<size_t>(p_end - q), sfn, slot);
            }
          }
          q += sizeof(scf_uci_harq_f2_f3_f4) + num_bytes;
        }

        if ((pucch_hdr.pdu_bitmap & 0x4U) != 0U && q + sizeof(scf_uci_csi_part) <= p_end) {
          const auto& csi1_hdr = *reinterpret_cast<const scf_uci_csi_part*>(q);
          const unsigned num_bytes = (csi1_hdr.bit_len + 7U) / 8U;
          if (q + sizeof(scf_uci_csi_part) + num_bytes <= p_end) {
            if (auto payload =
                    decode_payload_bits("UCI CSI-Part1", csi1_hdr.payload, csi1_hdr.bit_len, sfn, slot, pucch_hdr.rnti)) {
              uci.csi_part1 = ocudu::fapi::uci_csi_part1{
                  .detection_status    = decode_detection_status(csi1_hdr.crc),
                  .expected_bit_length = ocudu::units::bits(csi1_hdr.bit_len),
                  .payload             = std::move(*payload)};
            }
          }
          q += sizeof(scf_uci_csi_part) + num_bytes;
        }

        if ((pucch_hdr.pdu_bitmap & 0x8U) != 0U && q + sizeof(scf_uci_csi_part) <= p_end) {
          const auto& csi2_hdr = *reinterpret_cast<const scf_uci_csi_part*>(q);
          const unsigned num_bytes = (csi2_hdr.bit_len + 7U) / 8U;
          if (q + sizeof(scf_uci_csi_part) + num_bytes <= p_end) {
            if (auto payload =
                    decode_payload_bits("UCI CSI-Part2", csi2_hdr.payload, csi2_hdr.bit_len, sfn, slot, pucch_hdr.rnti)) {
              uci.csi_part2 = ocudu::fapi::uci_csi_part2{
                  .detection_status    = decode_detection_status(csi2_hdr.crc),
                  .expected_bit_length = ocudu::units::bits(csi2_hdr.bit_len),
                  .payload             = std::move(*payload)};
            }
          }
        }
        notifier.on_uci_indication(ind);
        g_bridge_runtime_metrics.uci_total.fetch_add(1, std::memory_order_relaxed);
        g_bridge_runtime_metrics.uci_pucch234.fetch_add(1, std::memory_order_relaxed);
        if (should_log_indication(g_uci_dispatch_count)) {
          std::fprintf(stderr,
                       "[aerial_bridge] UCI.indication #%u sfn=%u slot=%u type=PUCCH234 fmt=%u rnti=0x%04x sr_bits=%u harq=%u csi1=%u csi2=%u\n",
                       g_uci_dispatch_count.load(std::memory_order_relaxed),
                       sfn,
                       slot,
                       static_cast<unsigned>(pucch_hdr.pucch_format),
                       static_cast<unsigned>(pucch_hdr.rnti),
                       uci.sr.has_value() ? static_cast<unsigned>(uci.sr->sr_payload.size()) : 0U,
                       uci.harq.has_value() ? 1U : 0U,
                       uci.csi_part1.has_value() ? 1U : 0U,
                       uci.csi_part2.has_value() ? 1U : 0U);
        }
        break;
      }

      default:
        break;
    }

    p = p_end;
  }
}

// ═══════════════════════════════════════════════════════════════
// SRS_INDICATION deserialization (stub — SRS has complex IQ data)
// ═══════════════════════════════════════════════════════════════

void dispatch_srs_indication(const uint8_t* buf, size_t buf_len,
                             unsigned numerology,
                             ocudu::fapi::p7_indications_notifier& notifier)
{
  if (buf_len < SCF_HDR_SIZE + 5) return;

  const uint8_t* p = buf + SCF_HDR_SIZE;
  uint16_t sfn, slot;
  std::memcpy(&sfn, p, 2); p += 2;
  std::memcpy(&slot, p, 2); p += 2;
  const uint8_t num_pdus = *p++;

  const ocudu::slot_point sp(numerology, sfn, slot);

  for (uint8_t i = 0; i != num_pdus; ++i) {
    if (p + sizeof(scf_srs_ind_pdu_start) > buf + buf_len) {
      break;
    }

    const auto& srs_hdr = *reinterpret_cast<const scf_srs_ind_pdu_start*>(p);
    p += sizeof(scf_srs_ind_pdu_start);

    ocudu::fapi::srs_indication ind;
    ind.slot                     = sp;
    ind.pdu.handle               = srs_hdr.handle;
    ind.pdu.rnti                 = static_cast<ocudu::rnti_t>(srs_hdr.rnti);
    ind.pdu.timing_advance_offset = decode_timing_advance(srs_hdr.timing_advance, numerology);

    for (uint8_t sym = 0; sym != srs_hdr.num_reported_symbols; ++sym) {
      if (p + sizeof(scf_srs_ind_pdu_end) > buf + buf_len) {
        p = buf + buf_len;
        break;
      }
      const auto& report = *reinterpret_cast<const scf_srs_ind_pdu_end*>(p);
      p += sizeof(scf_srs_ind_pdu_end) + report.num_rbs;
      if (p > buf + buf_len) {
        p = buf + buf_len;
        break;
      }
    }

    notifier.on_srs_indication(ind);
    g_bridge_runtime_metrics.srs_total.fetch_add(1, std::memory_order_relaxed);
  }
}

} // namespace aerial_bridge
