// SPDX-FileCopyrightText: Copyright (c) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
// Aerial FAPI Bridge — Standalone Connection Test
//
// Connects to a running l2_adapter_cuphycontroller_scf via nvIPC,
// receives SLOT_INDICATIONs, and sends minimal FAPI responses.
// This validates the nvIPC transport layer without needing full OCUDU.

#include "aerial_bridge/nvipc_transport.h"
#include "aerial_bridge/msg_translator.h"

extern "C" {
#include "nv_ipc.h"
}

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>

static std::atomic<bool> g_running{true};

static void signal_handler(int /*sig*/)
{
  g_running = false;
}

int main()
{
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
  std::setvbuf(stdout, nullptr, _IONBF, 0); // Unbuffered stdout

  std::printf("============================================\n");
  std::printf("  Aerial FAPI Bridge — Connection Test\n");
  std::printf("============================================\n\n");

  // ── Connect to L1 via nvIPC ──────────────────────────────

  aerial_bridge::nvipc_transport transport;
  aerial_bridge::nvipc_transport_config cfg;
  cfg.prefix = "nvipc";

  std::printf("[bridge_test] Connecting to nvIPC (prefix: %s)...\n", cfg.prefix.c_str());

  if (!transport.connect(cfg)) {
    std::fprintf(stderr, "[bridge_test] ERROR: Failed to connect to nvIPC. Is L1 running?\n");
    return 1;
  }

  std::printf("[bridge_test] Connected to nvIPC successfully!\n\n");

  // ── Send CONFIG_REQUEST to L1 ────────────────────────────
  // Complete CONFIG with all mandatory TLVs for n78 TDD 100MHz 30kHz 2T2R.

  // Helper: append a TLV to the buffer, pad value to 4-byte boundary.
  // NOTE: Aerial L1 is built WITHOUT SCF_FAPI_10_04, so TLV length is uint16_t (2 bytes).
  auto add_tlv = [](uint8_t* p, uint16_t tag, uint16_t len, const void* val) -> uint8_t* {
    std::memcpy(p, &tag, 2); p += 2;
    std::memcpy(p, &len, 2); p += 2;  // 2-byte length (not 4!)
    std::memcpy(p, val, len);
    uint32_t padded = ((len + 3) / 4) * 4;
    if (padded > len) std::memset(p + len, 0, padded - len);
    p += padded;
    return p;
  };

  std::printf("[bridge_test] Step 1: Sending CONFIG_REQUEST...\n");
  {
    nv_ipc_msg_t tx_msg;
    std::memset(&tx_msg, 0, sizeof(tx_msg));
    tx_msg.msg_id   = aerial_bridge::scf_msg_id::CONFIG_REQUEST;
    tx_msg.cell_id  = 0;
    tx_msg.msg_len  = 512;  // Will be updated to actual size before send
    tx_msg.data_len = 0;

    if (transport.tx_allocate(tx_msg) == 0) {
      auto* buf = static_cast<uint8_t*>(tx_msg.msg_buf);
      std::memset(buf, 0, 1024);

      // FAPI header (2 bytes)
      buf[0] = 1;  // message_count
      buf[1] = 0;  // handle_id

      // Skip body header (6 bytes) — fill length at the end
      uint8_t* body = buf + 8;

      // num_tlvs byte at start of body, TLVs follow
      *body++ = 30; // num_tlvs (9 carrier + 2 grid + 7 SSB + 10 PRACH + 2 TDD)

      // ── Carrier config (9 TLVs) ──
      uint16_t v16; uint32_t v32; uint8_t v8; int32_t vi32;

      v16 = 273;                                         // 100 MHz @ 30kHz SCS
      body = add_tlv(body, 0x1001, 2, &v16);             // DL_BANDWIDTH
      v32 = 3600000;
      body = add_tlv(body, 0x1002, 4, &v32);             // DL_FREQ (kHz)
      v16 = 2;
      body = add_tlv(body, 0x1005, 2, &v16);             // NUM_TX_ANT
      v16 = 273;
      body = add_tlv(body, 0x1006, 2, &v16);             // UL_BANDWIDTH
      v32 = 3600000;
      body = add_tlv(body, 0x1007, 4, &v32);             // UL_FREQ (kHz)
      v16 = 2;
      body = add_tlv(body, 0x100A, 2, &v16);             // NUM_RX_ANT
      // DL/UL grid size: 5 × uint16_t for each numerology (mu 0-4)
      // For mu=1 (30kHz), set entry[1] = 273 PRBs
      uint16_t dl_grid[5] = {0, 273, 0, 0, 0};
      body = add_tlv(body, 0x1004, 10, dl_grid);          // DL_GRID_SIZE
      uint16_t ul_grid[5] = {0, 273, 0, 0, 0};
      body = add_tlv(body, 0x1009, 10, ul_grid);          // UL_GRID_SIZE
      v16 = 1;
      body = add_tlv(body, 0x100C, 2, &v16);             // PHY_CELL_ID
      v8 = 1;
      body = add_tlv(body, 0x100D, 1, &v8);              // FRAME_DUPLEX_TYPE (TDD)
      v8 = 1;
      body = add_tlv(body, 0x1010, 1, &v8);              // SCS_COMMON (30kHz, mu=1)

      // ── SSB config (7 TLVs) ──
      vi32 = 0;
      body = add_tlv(body, 0x100E, 4, &vi32);            // SSB_PBCH_POWER
      v16 = 0;
      body = add_tlv(body, 0x101D, 2, &v16);             // SSB_OFFSET_POINT_A
      v8 = 2;
      body = add_tlv(body, 0x101F, 1, &v8);              // SSB_PERIOD (20ms)
      v8 = 0;
      body = add_tlv(body, 0x1020, 1, &v8);              // SSB_SUBCARRIER_OFFSET
      uint8_t mib[3] = {0, 0, 0};
      body = add_tlv(body, 0x1021, 3, mib);              // MIB
      v32 = 1;
      body = add_tlv(body, 0x1022, 4, &v32);             // SSB_MASK[0]
      v32 = 0;
      body = add_tlv(body, 0x1022, 4, &v32);             // SSB_MASK[1]

      // ── PRACH config (10 TLVs) ──
      // ORDER MATTERS: ROOT_SEQ_INDEX increments the per-FD-occasion index in L1
      v8 = 1;
      body = add_tlv(body, 0x1011, 1, &v8);              // PRACH_SEQ_LEN (long)
      v8 = 1;
      body = add_tlv(body, 0x1012, 1, &v8);              // PRACH_SUBC_SPACING (30kHz)
      v8 = 0;
      body = add_tlv(body, 0x1013, 1, &v8);              // RESTRICTED_SET_CONFIG (unrestricted)
      v8 = 159;
      body = add_tlv(body, 0x1029, 1, &v8);              // PRACH_CONFIG_INDEX
      v8 = 1;
      body = add_tlv(body, 0x1014, 1, &v8);              // NUM_PRACH_FD_OCCASIONS
      // Per-FD-occasion block (1 occasion):
      v16 = 1;
      body = add_tlv(body, 0x1015, 2, &v16);             // PRACH_ROOT_SEQ_INDEX (triggers fd_index++)
      v8 = 1;
      body = add_tlv(body, 0x1016, 1, &v8);              // NUM_ROOT_SEQ
      v16 = 0;
      body = add_tlv(body, 0x1017, 2, &v16);             // K1 (freq offset)
      v8 = 0;
      body = add_tlv(body, 0x1018, 1, &v8);              // PRACH_ZERO_CORR_CONF
      v8 = 0;
      body = add_tlv(body, 0x101B, 1, &v8);              // SSB_PER_RACH

      // ── TDD config (2 TLVs) ──
      v8 = 6;
      body = add_tlv(body, 0x1026, 1, &v8);              // TDD_PERIOD (5ms)

      // SLOT_CONFIG: single TLV, flat array of 10 slots × 14 symbols = 140 bytes
      // DDDSU×2 pattern: 0=DL, 1=UL, 2=flexible
      uint8_t slot_cfg[140];
      for (int s = 0; s < 10; s++) {
        uint8_t* slot = &slot_cfg[s * 14];
        if (s == 3 || s == 8) {
          // Special slot: 10 DL + 2 guard + 2 UL
          std::memset(slot, 0, 10);
          slot[10] = 2; slot[11] = 2;
          slot[12] = 1; slot[13] = 1;
        } else if (s == 4 || s == 9) {
          // UL slot
          std::memset(slot, 1, 14);
        } else {
          // DL slot
          std::memset(slot, 0, 14);
        }
      }
      body = add_tlv(body, 0x1027, 140, slot_cfg);       // SLOT_CONFIG

      // Fill body header
      uint16_t type_id = aerial_bridge::scf_msg_id::CONFIG_REQUEST;
      std::memcpy(buf + 2, &type_id, 2);
      uint32_t body_len = static_cast<uint32_t>(body - (buf + 8));
      std::memcpy(buf + 4, &body_len, 4);
      tx_msg.msg_len = static_cast<int>(body - buf);

      // Hex dump for debugging
      std::printf("[bridge_test] CONFIG hex dump (%d bytes):\n", tx_msg.msg_len);
      for (int i = 0; i < tx_msg.msg_len && i < 80; ++i) {
        std::printf("%02x ", buf[i]);
        if ((i + 1) % 16 == 0) std::printf("\n");
      }
      std::printf("\n");

      __sync_synchronize(); // Memory barrier for ARM shared memory
      transport.tx_send(tx_msg);
      transport.tx_sem_post();
      std::printf("[bridge_test] CONFIG_REQUEST sent (%d bytes, 30 TLVs)\n",
                  tx_msg.msg_len);
    } else {
      std::fprintf(stderr, "[bridge_test] Failed to allocate CONFIG_REQUEST buffer\n");
    }
  }

  // Wait for CONFIG_RESPONSE
  std::printf("[bridge_test] Waiting for CONFIG_RESPONSE...\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(2000)); // L1 cell creation takes time
  {
    transport.rx_sem_wait();
    nv_ipc_msg_t rx_msg;
    while (transport.rx_recv(rx_msg) == 0) {
      if (rx_msg.msg_id == aerial_bridge::scf_msg_id::CONFIG_RESPONSE) {
        auto* rbuf = static_cast<const uint8_t*>(rx_msg.msg_buf);
        uint8_t error_code = (rx_msg.msg_len >= 9) ? rbuf[8] : 0xFF;
        std::printf("[bridge_test] CONFIG_RESPONSE: error_code=0x%02x %s\n",
                    error_code, error_code == 0 ? "(OK)" : "(FAILED)");
      } else {
        std::printf("[bridge_test] Received message type 0x%02x (cell=%d, len=%d)\n",
                    rx_msg.msg_id, rx_msg.cell_id, rx_msg.msg_len);
      }
      transport.rx_release(rx_msg);
    }
  }

  // ── Send START_REQUEST ───────────────────────────────────

  std::printf("\n[bridge_test] Step 2: Sending START_REQUEST...\n");
  {
    nv_ipc_msg_t tx_msg;
    std::memset(&tx_msg, 0, sizeof(tx_msg));
    tx_msg.msg_id   = aerial_bridge::scf_msg_id::START_REQUEST;
    tx_msg.cell_id  = 0;
    tx_msg.msg_len  = 8;
    tx_msg.data_len = 0;

    if (transport.tx_allocate(tx_msg) == 0) {
      auto* buf = static_cast<uint8_t*>(tx_msg.msg_buf);
      std::memset(buf, 0, 8);
      buf[0] = 1; // message_count
      buf[1] = 0; // cell_id
      uint16_t type_id = aerial_bridge::scf_msg_id::START_REQUEST;
      std::memcpy(buf + 2, &type_id, 2);
      uint32_t body_len = 0;
      std::memcpy(buf + 4, &body_len, 4);
      transport.tx_send(tx_msg);
      transport.tx_sem_post();
      std::printf("[bridge_test] START_REQUEST sent\n");
    }
  }

  std::printf("\n[bridge_test] Step 3: Listening for SLOT_INDICATIONs...\n\n");

  // ── Main loop: receive and log FAPI messages ─────────────

  unsigned slot_count = 0;
  unsigned msg_count  = 0;
  unsigned error_count = 0;
  uint16_t current_sfn  = 0;
  uint16_t current_slot = 0;
  auto start_time = std::chrono::steady_clock::now();

  while (g_running) {
    // Wait for L1 to signal a new TTI.
    transport.rx_sem_wait();

    if (!g_running) break;

    // Drain all queued messages.
    nv_ipc_msg_t msg;
    while (transport.rx_recv(msg) == 0) {
      msg_count++;

      const char* msg_name = "UNKNOWN";
      switch (msg.msg_id) {
        case aerial_bridge::scf_msg_id::SLOT_INDICATION:
          msg_name = "SLOT_INDICATION";
          slot_count++;

          // Parse SFN/slot and track for responses.
          if (msg.msg_len >= 12) {
            const auto* buf = static_cast<const uint8_t*>(msg.msg_buf);
            std::memcpy(&current_sfn, buf + 8, 2);
            std::memcpy(&current_slot, buf + 10, 2);

            if (slot_count <= 10 || slot_count % 1000 == 0) {
              std::printf("[bridge_test] SLOT_IND #%u: SFN=%u Slot=%u\n",
                          slot_count, current_sfn, current_slot);
            }
          }
          break;

        case aerial_bridge::scf_msg_id::ERROR_INDICATION:
          msg_name = "ERROR_INDICATION";
          error_count++;
          if (error_count <= 5) {
            std::printf("[bridge_test] ERROR_INDICATION received (cell=%d)\n", msg.cell_id);
          }
          break;

        case aerial_bridge::scf_msg_id::PARAM_RESPONSE:
          msg_name = "PARAM_RESPONSE";
          std::printf("[bridge_test] PARAM_RESPONSE received\n");
          break;

        case aerial_bridge::scf_msg_id::CONFIG_RESPONSE:
          msg_name = "CONFIG_RESPONSE";
          std::printf("[bridge_test] CONFIG_RESPONSE received\n");
          break;

        case aerial_bridge::scf_msg_id::STOP_INDICATION:
          msg_name = "STOP_INDICATION";
          std::printf("[bridge_test] STOP_INDICATION received\n");
          break;

        default:
          std::printf("[bridge_test] Message type 0x%02x (cell=%d, len=%d)\n",
                      msg.msg_id, msg.cell_id, msg.msg_len);
          break;
      }

      transport.rx_release(msg);
    }

    // Respond with empty DL_TTI + UL_TTI using the correct SFN/slot.
    // This keeps L1 happy (no SFN mismatch errors).
    if (slot_count > 0) {
      // Helper lambda to send an empty FAPI request with correct SFN/slot.
      auto send_empty_request = [&](uint16_t type_id) {
        nv_ipc_msg_t tx_msg;
        std::memset(&tx_msg, 0, sizeof(tx_msg));
        tx_msg.msg_id   = type_id;
        tx_msg.cell_id  = 0;
        tx_msg.msg_len  = 64;
        tx_msg.data_len = 0;

        if (transport.tx_allocate(tx_msg) == 0) {
          auto* buf = static_cast<uint8_t*>(tx_msg.msg_buf);
          std::memset(buf, 0, 64);
          // FAPI header
          buf[0] = 1;  // message_count
          buf[1] = 0;  // cell_id
          // Body header
          std::memcpy(buf + 2, &type_id, 2);
          uint32_t body_len = 6;
          std::memcpy(buf + 4, &body_len, 4);
          // Body: sfn + slot + num_pdus=0 + ngroup=0
          std::memcpy(buf + 8, &current_sfn, 2);
          std::memcpy(buf + 10, &current_slot, 2);
          buf[12] = 0; // num_pdus
          buf[13] = 0; // ngroup
          tx_msg.msg_len = 14;
          transport.tx_send(tx_msg);
        }
      };

      // Send empty DL_TTI and UL_TTI for this slot.
      send_empty_request(aerial_bridge::scf_msg_id::DL_TTI_REQUEST);
      send_empty_request(aerial_bridge::scf_msg_id::UL_TTI_REQUEST);

      // Signal L1 that all slot messages are sent.
      transport.tx_sem_post();
    }
  }

  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - start_time);

  std::printf("\n============================================\n");
  std::printf("  Test Summary\n");
  std::printf("============================================\n");
  std::printf("  Duration:       %ld seconds\n", elapsed.count());
  std::printf("  Messages:       %u\n", msg_count);
  std::printf("  Slot Indications: %u\n", slot_count);
  std::printf("  Error Indications: %u\n", error_count);
  if (elapsed.count() > 0) {
    std::printf("  Slots/sec:      %.1f\n",
                static_cast<double>(slot_count) / elapsed.count());
  }
  std::printf("============================================\n");

  transport.disconnect();
  return 0;
}
