<!-- Copyright (c) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Aerial FAPI Bridge Plugin — Design Document

## 1. Overview

The Aerial FAPI Bridge Plugin connects OCUDU's L2/L3 stack (MAC, RLC, PDCP, RRC, NGAP) to
NVIDIA Aerial's GPU-accelerated L1 (cuPHY) via the SCF 5G FAPI interface. It implements
OCUDU's `split6_plugin` interface and communicates with the Aerial L1 over NVIDIA's nvIPC
shared-memory transport.

### 1.1 Goals

- Run OCUDU's open-source L2/L3 with Aerial's high-performance GPU L1
- Use SCF FAPI as the standardized L1/L2 interface
- Minimize latency: in-process plugin (no extra IPC hop between OCUDU and bridge)
- Support DGX Spark (GB10) with CUDA 12.9 and ARM Neoverse V3

### 1.2 Non-Goals (Phase 1)

- GPU zero-copy data path (Phase 4)
- Multi-cell / multi-sector support (Phase 4)
- DPDK/DOCA transport (SHM only for now)

## 2. Architecture

```
┌──────────────────────────────────────────────┐
│              OCUDU gnb_split_6               │
│                                              │
│  RRC ─── PDCP ─── RLC ─── MAC/Scheduler     │
│                              │               │
│                    mac_fapi_adaptor           │
│                    (existing OCUDU code)      │
│                              │               │
│                    C++ virtual calls          │
│                              │               │
│  ┌───────────────────────────▼────────────┐  │
│  │      AERIAL FAPI BRIDGE PLUGIN         │  │
│  │                                        │  │
│  │  ┌─────────────────────────────────┐   │  │
│  │  │  aerial_split6_plugin           │   │  │
│  │  │  (split6_plugin impl)           │   │  │
│  │  └───────────┬─────────────────────┘   │  │
│  │              │ creates                 │  │
│  │  ┌───────────▼─────────────────────┐   │  │
│  │  │  aerial_fapi_adaptor            │   │  │
│  │  │  (phy_fapi_adaptor impl)        │   │  │
│  │  │                                 │   │  │
│  │  │  ┌───────────────────────────┐  │   │  │
│  │  │  │ aerial_sector_adaptor     │  │   │  │
│  │  │  │                           │  │   │  │
│  │  │  │  P5: config/start/stop ──────────────── CONFIG_REQ ──┐
│  │  │  │  P7: dl_tti/ul_tti/tx ───────────────── DL_TTI_REQ ──┤
│  │  │  │  last_req: sem_post ─────────────────── SEM_POST ────┤
│  │  │  │                           │  │   │  │               │
│  │  │  │  RX thread ◄────────────────────────── SLOT_IND ◄───┤
│  │  │  │  (slot_ind, crc, rx,  ◄──────────────── CRC_IND ◄───┤ nvIPC
│  │  │  │   uci, rach, srs)    ◄───────────────── RX_DATA ◄───┤ SHM
│  │  │  └───────────────────────────┘  │   │  │               │
│  │  └─────────────────────────────────┘   │  │               │
│  │                                        │  │               │
│  │  ┌─────────────────────────────────┐   │  │               │
│  │  │  aerial_nvipc_transport         │   │  │               │
│  │  │  (nvIPC lifecycle management)   ├───────────────────────┘
│  │  └─────────────────────────────────┘   │  │
│  └────────────────────────────────────────┘  │
└──────────────────────────────────────────────┘
                    │
                    │ nvIPC shared memory (/dev/shm/nvipc_*)
                    │
┌───────────────────▼──────────────────────────┐
│           Aerial cuPHY L1                    │
│           (cuphycontroller_scf)              │
│                                              │
│  cuPHY (GPU) ── cuMAC ── aerial-fh-driver    │
│  LDPC enc/dec   MAC CP    DPDK fronthaul     │
│  channel est    HARQ      O-RAN fronthaul    │
│  FFT/IFFT       sched     compression        │
└──────────────────────────────────────────────┘
```

## 3. Component Design

### 3.1 Class Hierarchy

```
split6_plugin (OCUDU interface)
  └── aerial_split6_plugin
        └── creates aerial_fapi_adaptor

phy_fapi_adaptor (OCUDU interface)
  └── aerial_fapi_adaptor
        └── owns aerial_sector_adaptor(s)

phy_fapi_sector_adaptor (OCUDU interface)
  └── aerial_sector_adaptor
        ├── phy_fapi_p5_sector_adaptor → aerial_p5_adaptor
        │     └── p5_requests_gateway → aerial_p5_gateway
        └── phy_fapi_p7_sector_adaptor → aerial_p7_adaptor
              ├── p7_requests_gateway → aerial_p7_gateway
              └── p7_last_request_notifier → aerial_last_request_notifier

Transport layer (standalone):
  └── aerial_nvipc_transport (connects to L1, manages buffers)
  └── aerial_rx_thread (polls nvIPC, dispatches indications)
```

### 3.2 Data Flow

**Downlink (OCUDU → Aerial):**
1. OCUDU MAC scheduler produces results
2. `mac_fapi_adaptor` translates to OCUDU FAPI C++ objects
3. Calls `aerial_p7_gateway::send_dl_tti_request(ocudu::fapi::dl_tti_request)`
4. Gateway allocates nvIPC TX buffer from CPU_MSG pool
5. Serializes OCUDU C++ struct → packed SCF `scf_fapi_dl_tti_req_t` in buffer
6. Calls `ipc->tx_send_msg()` to enqueue for L1
7. After all messages: `aerial_last_request_notifier::on_last_message()` → `ipc->tx_tti_sem_post()`
8. L1 wakes, processes the DL/UL requests on GPU

**Uplink (Aerial → OCUDU):**
1. L1 completes UL processing, posts indication via nvIPC
2. `aerial_rx_thread` wakes on `ipc->rx_tti_sem_wait()`
3. Receives messages via `ipc->rx_recv_msg()`
4. Deserializes SCF C struct → OCUDU C++ object
5. Calls appropriate OCUDU notifier:
   - `SLOT_INDICATION` → `p7_slot_indication_notifier::on_slot_indication()`
   - `CRC_INDICATION` → `p7_indications_notifier::on_crc_indication()`
   - `RX_DATA_INDICATION` → `p7_indications_notifier::on_rx_data_indication()`
   - etc.
6. Releases nvIPC buffer via `ipc->rx_release()`

### 3.3 Threading Model

| Thread | Affinity | Priority | Responsibility |
|--------|----------|----------|----------------|
| RX thread | Pinned core | RT 90 | nvIPC poll → OCUDU notifier dispatch |
| OCUDU workers | OCUDU-managed | Normal | Scheduling, RLC, PDCP processing |
| TX path | OCUDU thread | Normal | Runs in caller's context (lock-free nvIPC send) |

### 3.4 Memory Management

The bridge uses nvIPC's pre-allocated memory pools:

| Pool | Size | Use |
|------|------|-----|
| CPU_MSG | ~15KB × 4096 | FAPI message headers |
| CPU_DATA | ~576KB × 1024 | Transport block data |
| CUDA_DATA | ~307KB × 0 | GPU buffers (Phase 4) |

TX path: allocate from pool → serialize → send → pool auto-recycles
RX path: receive (pool-allocated) → deserialize → notify OCUDU → release

## 4. Message Translation

### 4.1 SLOT_INDICATION (Aerial → OCUDU)

```
SCF: scf_fapi_slot_ind_t { sfn: uint16_t, slot: uint16_t }
  → OCUDU: fapi::slot_indication { slot: slot_point_extended, time_point }

Translation:
  slot_point sp(numerology, sfn, slot);
  slot_point_extended spe(sp);
  return slot_indication{spe, std::chrono::system_clock::now()};
```

### 4.2 DL_TTI_REQUEST (OCUDU → Aerial)

```
OCUDU: fapi::dl_tti_request { slot, pdus: vector<dl_tti_request_pdu> }
  → SCF: scf_fapi_dl_tti_req_t { sfn, slot, nPDUs, nGroup, pdu[] }

Translation:
  For each PDU in request:
    - PDCCH → SCF_FAPI_DL_TTI_PDU_TYPE_PDCCH
    - PDSCH → SCF_FAPI_DL_TTI_PDU_TYPE_PDSCH
    - SSB   → SCF_FAPI_DL_TTI_PDU_TYPE_SSB
    - CSI-RS → SCF_FAPI_DL_TTI_PDU_TYPE_CSI_RS
  Pack into contiguous buffer with variable-length PDU array
```

### 4.3 CONFIG_REQUEST (OCUDU → Aerial)

```
OCUDU: fapi::config_request { cell_config with carrier, SSB, PRACH, TDD params }
  → SCF: scf_fapi_config_request_msg_t { TLV array with tag/length/value }

Translation:
  Build TLV list from cell_config fields:
    - TAG_CARRIER_CONFIG: bandwidth, frequency, grid_size, num_ant
    - TAG_CELL_CONFIG: pci, duplex_type
    - TAG_SSB_CONFIG: scs, pbch_power
    - TAG_PRACH_CONFIG: seq_length, scs, root_seq
    - etc.
```

## 5. Configuration

### 5.1 Bridge YAML Configuration

```yaml
aerial_bridge:
  nvipc:
    transport: shm
    prefix: "nvipc"
  rx_thread:
    cpu_core: 4
    priority: 90
  numerology: 1        # SCS 30kHz
  log_level: info
```

### 5.2 Aerial L1 Configuration

Standard `cuphycontroller_scf` YAML with matching nvIPC prefix.

### 5.3 OCUDU Configuration

Standard `gnb_split_6` YAML with cell parameters matching L1 capabilities.

## 6. Startup Sequence

1. Start Aerial L1: `cuphycontroller_scf --config l1_config.yaml`
   - L1 creates nvIPC primary instance, waits for secondary
2. Start OCUDU: `gnb_split_6 -c gnb_config.yaml`
   - Plugin creates `aerial_fapi_adaptor`
   - `aerial_nvipc_transport` connects as secondary
   - `aerial_rx_thread` starts polling
3. OCUDU sends CONFIG_REQUEST → L1 responds CONFIG_RESPONSE
4. OCUDU sends START_REQUEST → L1 begins SLOT_INDICATION flow
5. Normal operation: slot-driven DL/UL scheduling

## 7. Error Handling

- **nvIPC connection failure**: Retry with backoff, log error
- **Message serialization failure**: Log and skip (don't crash)
- **L1 ERROR_INDICATION**: Forward to OCUDU error notifier
- **Slot timing violation**: L1 reports via ERROR_INDICATION, bridge logs
- **L1 crash**: RX thread detects broken SHM, triggers OCUDU shutdown

## 8. Build System

The bridge is a standalone CMake project that links against:
- OCUDU headers (FAPI interfaces)
- Aerial `libnvipc.so` and `scf_5g_fapi.h`

It produces a static library that replaces the dummy split6 plugin
in the OCUDU `gnb_split_6` build.

## 9. Implementation Status

### Implemented

- split-6 plugin registration and CLI integration
- nvIPC transport lifecycle and RX thread dispatch
- P5 request/response path used by the current OCUDU configuration
- P7 request serialization for `DL_TTI`, `UL_TTI`, `UL_DCI`, and `TX_DATA`
- indication parsing for `SLOT`, `CRC`, `RX_DATA`, `UCI`, `RACH`, and `SRS`
- no-NIC runtime hardening required for GB10/Aerial `25.3.2`
- characterization reporting for runtime health, message coverage, and channel coverage

### Validated In Current Release

- stable no-NIC `10s`, `20s`, and `60s` OCUDU-to-Aerial reruns on GB10
- sustained `PDSCH`, `PUSCH`, `PUCCH F0/F1`, `CRC`, `UCI`, and `RX_DATA` message flow
- no slot-jump, queue-saturation, aggregate-exhaustion, or `Err.ind` failures in the latest reference reruns

### Remaining Release Gaps

- dynamic derivation of the full FAPI config from OCUDU runtime config
- explicit signoff for `PUCCH F2/F3/F4`, `PRACH`, and `SRS` in targeted runs
- true UL PHY-quality characterization with a real waveform source or explicit PHY loopback mode
- broader recovery/soak automation for long-duration release testing

## 10. Testing Strategy

### Phase 1 Verification
1. Plugin loads and connects to nvIPC
2. SLOT_INDICATION flows from L1 to OCUDU
3. CONFIG/START handshake completes
4. Logs show timing and message flow

### Full Integration
1. Stable no-NIC FAPI handshake and slot flow
2. Sustained scheduler-driven DL/UL traffic
3. Metrics extraction via `scripts/characterize_integration.py`
4. Targeted channel-specific reruns for remaining unexercised message types
