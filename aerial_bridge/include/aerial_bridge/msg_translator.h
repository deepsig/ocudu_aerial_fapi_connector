// SPDX-FileCopyrightText: Copyright (c) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
// Aerial FAPI Bridge — Message Translator
//
// Stateless translation functions between OCUDU C++ FAPI objects and
// SCF FAPI packed C structs matching the non-SCF_FAPI_10_04 layout used by
// the Aerial build described in this repository.

#pragma once

#include <cstddef>
#include <cstdint>

// Forward declarations — avoid pulling in heavy OCUDU/SCF headers here.
namespace ocudu {
class slot_point;
namespace fapi {
class p7_indications_notifier;
struct dl_tti_request;
struct ul_tti_request;
struct ul_dci_request;
struct tx_data_request;
struct slot_indication;
struct crc_indication;
struct rx_data_indication;
struct uci_indication;
struct rach_indication;
struct srs_indication;
} // namespace fapi
} // namespace ocudu

namespace aerial_bridge {

// ── FAPI message type IDs (from SCF spec) ───────────────────

namespace scf_msg_id {
constexpr uint16_t PARAM_REQUEST      = 0x00;
constexpr uint16_t PARAM_RESPONSE     = 0x01;
constexpr uint16_t CONFIG_REQUEST     = 0x02;
constexpr uint16_t CONFIG_RESPONSE    = 0x03;
constexpr uint16_t START_REQUEST      = 0x04;
constexpr uint16_t STOP_REQUEST       = 0x05;
constexpr uint16_t STOP_INDICATION    = 0x06;
constexpr uint16_t ERROR_INDICATION   = 0x07;
constexpr uint16_t DL_TTI_REQUEST     = 0x80;
constexpr uint16_t UL_TTI_REQUEST     = 0x81;
constexpr uint16_t SLOT_INDICATION    = 0x82;
constexpr uint16_t UL_DCI_REQUEST     = 0x83;
constexpr uint16_t TX_DATA_REQUEST    = 0x84;
constexpr uint16_t RX_DATA_INDICATION = 0x85;
constexpr uint16_t CRC_INDICATION     = 0x86;
constexpr uint16_t UCI_INDICATION     = 0x87;
constexpr uint16_t SRS_INDICATION     = 0x88;
constexpr uint16_t RACH_INDICATION    = 0x89;
} // namespace scf_msg_id

// ── SCF FAPI PDU type IDs ───────────────────────────────────

namespace scf_pdu_type {
// DL TTI PDU types
constexpr uint16_t PDCCH  = 0;
constexpr uint16_t PDSCH  = 1;
constexpr uint16_t CSI_RS = 2;
constexpr uint16_t SSB    = 3;
// UL TTI PDU types
constexpr uint16_t PRACH  = 0;
constexpr uint16_t PUSCH  = 1;
constexpr uint16_t PUCCH  = 2;
constexpr uint16_t SRS    = 3;
} // namespace scf_pdu_type

// ── SCF FAPI header structures (packed, for wire format) ────

#pragma pack(push, 1)

/// FAPI message envelope header.
struct scf_header {
  uint8_t message_count; ///< Number of messages in this batch.
  uint8_t handle_id;     ///< PHY instance / cell ID.
};

/// FAPI message body header.
struct scf_body_header {
  uint16_t type_id; ///< Message type ID.
  uint32_t length;  ///< Length of message body (after this header).
};

/// Generic PDU info (precedes each PDU in DL_TTI / UL_TTI arrays).
struct scf_generic_pdu_info {
  uint16_t pdu_type;
  uint16_t pdu_size; ///< Size of this struct + PDU-specific data.
  uint8_t  pdu_config[0];
};

/// BWP parameters (common to many PDU types).
struct scf_bwp {
  uint16_t bwp_size;
  uint16_t bwp_start;
  uint8_t  scs;
  uint8_t  cyclic_prefix;
};

/// PDSCH codeword entry.
struct scf_pdsch_codeword {
  uint16_t target_code_rate;
  uint8_t  qam_mod_order;
  uint8_t  mcs_index;
  uint8_t  mcs_table;
  uint8_t  rv_index;
  uint32_t tb_size;
};

/// PDSCH trailing fields after the variable-length codeword array.
struct scf_pdsch_pdu_end {
  uint16_t data_scrambling_id;
  uint8_t  num_of_layers;
  uint8_t  transmission_scheme;
  uint8_t  ref_point;
  uint16_t dl_dmrs_sym_pos;
  uint8_t  dmrs_config_type;
  uint16_t dl_dmrs_scrambling_id;
  uint8_t  sc_id;
  uint8_t  num_dmrs_cdm_grps_no_data;
  uint16_t dmrs_ports;
  uint8_t  resource_alloc;
  uint8_t  rb_bitmap[36];
  uint16_t rb_start;
  uint16_t rb_size;
  uint8_t  vrb_to_prb_mapping;
  uint8_t  start_sym_index;
  uint8_t  num_symbols;
};

/// TX precoding / beamforming block that follows PDSCH/CSI-RS payloads.
struct scf_tx_precoding_beamforming {
  uint16_t num_prgs;
  uint16_t prg_size;
  uint8_t  dig_bf_interfaces;
  uint16_t pm_idx_and_beam_idx[0];
};

/// TX power block that follows the beamforming block.
struct scf_tx_power_info {
  uint8_t power_control_offset;
  uint8_t power_control_offset_ss;
};

/// DL DCI entry inside a PDCCH PDU.
struct scf_dl_dci {
  uint16_t rnti;
  uint16_t scrambling_id;
  uint16_t scrambling_rnti;
  uint8_t  cce_index;
  uint8_t  aggregation_level;
  uint8_t  payload[0];
};

/// PDCCH-specific tx power block that follows the DCI beamforming block.
struct scf_pdcch_tx_power_info {
  uint8_t beta_pdcch_1_0;
  uint8_t power_control_offset_ss;
};

/// PDCCH DCI payload header.
struct scf_pdcch_dci_payload {
  uint16_t payload_size_bits;
  uint8_t  payload[0];
};

/// RX beamforming block used by PRACH/PUSCH/SRS uplink PDUs.
struct scf_rx_beamforming {
  uint16_t num_prgs;
  uint16_t prg_size;
  uint8_t  dig_bf_interfaces;
  uint16_t beam_idx[0];
};

/// Optional PUSCH DFT-s-OFDM block.
struct scf_pusch_dft_ofdm {
  uint8_t  low_papr_group_number;
  uint16_t low_papr_sequence_number;
  uint8_t  ul_ptrs_sample_density;
  uint8_t  ul_ptrs_time_density_transform_precoding;
};

/// DL_TTI.request message body (after scf_body_header).
struct scf_dl_tti_req {
  uint16_t sfn;
  uint16_t slot;
  uint8_t  num_pdus;
  uint8_t  ngroup;
  uint8_t  payload[0]; ///< Array of scf_generic_pdu_info + PDU-specific data.
};

/// SSB PDU.
struct scf_ssb_pdu {
  uint16_t phys_cell_id;
  uint8_t  beta_pss;
  uint8_t  ssb_block_index;
  uint8_t  ssb_subcarrier_offset;
  uint16_t ssb_offset_pointA;
  uint8_t  bch_payload_flag;
  uint32_t bch_payload;
};

/// UL_TTI.request message body.
struct scf_ul_tti_req {
  uint16_t sfn;
  uint16_t slot;
  uint8_t  num_pdus;
  uint8_t  rach_present;
  uint8_t  num_ulsch;
  uint8_t  num_ulcch;
  uint8_t  ngroup;
  uint8_t  payload[0];
};

/// TX_DATA.request message body.
struct scf_tx_data_req {
  uint16_t sfn;
  uint16_t slot;
  uint16_t num_pdus;
  uint8_t  payload[0];
};

/// SLOT_INDICATION body.
struct scf_slot_ind {
  uint16_t sfn;
  uint16_t slot;
};

#pragma pack(pop)

// ── TX direction: OCUDU → SCF (serialize into nvIPC buffer) ─

/// Serialize a DL_TTI.request into a pre-allocated buffer.
/// @param msg     OCUDU DL TTI request.
/// @param buf     Output buffer (must be large enough).
/// @param buf_len Available buffer size.
/// @param cell_id Cell / PHY instance ID.
/// @return Number of bytes written, or -1 on error.
int serialize_dl_tti_request(const ocudu::fapi::dl_tti_request& msg,
                             uint8_t* buf, size_t buf_len, uint8_t cell_id);

/// Serialize a UL_TTI.request into a pre-allocated buffer.
int serialize_ul_tti_request(const ocudu::fapi::ul_tti_request& msg,
                             uint8_t* buf, size_t buf_len, uint8_t cell_id);

/// Serialize a UL_DCI.request into a pre-allocated buffer.
int serialize_ul_dci_request(const ocudu::fapi::ul_dci_request& msg,
                             uint8_t* buf, size_t buf_len, uint8_t cell_id);

/// Serialize a TX_DATA.request header into msg_buf.
/// Transport block data should be copied separately into data_buf.
/// @return Number of bytes written to msg_buf, or -1 on error.
int serialize_tx_data_request(const ocudu::fapi::tx_data_request& msg,
                              uint8_t* msg_buf, size_t msg_buf_len,
                              uint8_t* data_buf, size_t data_buf_len,
                              uint8_t cell_id,
                              size_t& data_bytes_written);

// ── RX direction: SCF → OCUDU (deserialize from nvIPC buffer)

/// Deserialize a SLOT_INDICATION from a raw buffer.
/// @param buf     Input buffer (after FAPI header).
/// @param buf_len Buffer length.
/// @param numerology SCS numerology for slot_point construction.
/// @param[out] ind Output OCUDU slot indication.
/// @return true on success.
bool deserialize_slot_indication(const uint8_t* buf, size_t buf_len,
                                 unsigned numerology,
                                 ocudu::fapi::slot_indication& ind);

/// Deserialize a CRC_INDICATION and call the notifier for each PDU.
/// @param buf     Raw nvIPC message buffer.
/// @param buf_len Buffer length.
/// @param numerology SCS numerology.
/// @param notifier OCUDU indications notifier to dispatch to.
void dispatch_crc_indication(const uint8_t* buf, size_t buf_len,
                             unsigned numerology,
                             ocudu::fapi::p7_indications_notifier& notifier);

/// Deserialize an RX_DATA_INDICATION and call the notifier for each PDU.
/// @note The buf must remain valid until the notifier returns (zero-copy span).
void dispatch_rx_data_indication(const uint8_t* buf, size_t buf_len,
                                 const uint8_t* data_buf, size_t data_len,
                                 unsigned numerology,
                                 ocudu::fapi::p7_indications_notifier& notifier);

/// Deserialize a RACH_INDICATION and call the notifier.
void dispatch_rach_indication(const uint8_t* buf, size_t buf_len,
                              unsigned numerology,
                              ocudu::fapi::p7_indications_notifier& notifier);

/// Deserialize a UCI_INDICATION and call the notifier for each PDU.
void dispatch_uci_indication(const uint8_t* buf, size_t buf_len,
                             unsigned numerology,
                             ocudu::fapi::p7_indications_notifier& notifier);

/// Deserialize a SRS_INDICATION and call the notifier.
void dispatch_srs_indication(const uint8_t* buf, size_t buf_len,
                             unsigned numerology,
                             ocudu::fapi::p7_indications_notifier& notifier);

void reset_bridge_runtime_metrics();
void dump_bridge_runtime_metrics();

} // namespace aerial_bridge
