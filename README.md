<!-- Copyright (c) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# OCUDU + Aerial: GPU-Accelerated 5G RAN on DGX Spark

Docker-based build and integration environment that combines [OCUDU](https://gitlab.com/ocudu/ocudu)'s open-source L2/L3 stack with [NVIDIA Aerial](https://github.com/NVIDIA/aerial-cuda-accelerated-ran)'s GPU-accelerated L1 (cuPHY) via a custom FAPI bridge plugin. Developed and tested on the NVIDIA DGX Spark (GB10 Blackwell desktop GPU).

## License

Unless a file states otherwise, this repository is Copyright DeepSig Inc. 2026
and distributed under the BSD-3-Clause-Clear license. See
[`LICENSE`](/home/deepsig/src/ocudu-aerial/LICENSE).
The vendored files under [`ocudu_patches`](/home/deepsig/src/ocudu-aerial/ocudu_patches)
retain their upstream notices in addition to DeepSig modification attribution.

## Architecture

```
┌──────────────────────────────────────┐
│     OCUDU gnb (L2/L3)               │
│  RRC / NGAP / PDCP / RLC / MAC      │
│           │                          │
│     mac_fapi_adaptor                 │
│           │ C++ virtual calls        │
│  ┌────────▼──────────────────────┐   │
│  │  AERIAL FAPI BRIDGE PLUGIN   │   │
│  │  libaerial_bridge.a           │   │
│  │  • OCUDU C++ ↔ SCF C structs │   │
│  │  • nvIPC SHM transport        │   │
│  │  • RX thread for indications  │   │
│  └────────┬──────────────────────┘   │
└───────────│──────────────────────────┘
            │ nvIPC shared memory
┌───────────▼──────────────────────────┐
│  Aerial cuPHY L1 (GPU)               │
│  cuphycontroller_scf                 │
│  • cuPHY GPU PHY (LDPC, FFT, etc.)  │
│  • SM_120 (Blackwell) CUDA kernels  │
│  • nvIPC FAPI endpoint               │
└──────────────────────────────────────┘
```

## Current Status (2026-04-10)

OCUDU's MAC scheduler now drives NVIDIA Aerial's GPU L1 over nvIPC without a NIC, and the current no-NIC integration path is stable in current reruns:

| Milestone | Status |
|-----------|--------|
| Docker build (Aerial + OCUDU + bridge) | ✅ Complete |
| CONFIG_REQUEST accepted (30 TLVs, error_code=0) | ✅ Complete |
| START_REQUEST accepted, L1 in RUNNING state | ✅ Complete |
| GPU cell created (LDPC, PDSCH, PUSCH, PRACH on SM_120) | ✅ Complete |
| SLOT_INDICATIONs flowing to gnb MAC scheduler | ✅ Complete |
| gnb generates DL_TTI/UL_TTI/TX_DATA with transport blocks | ✅ Complete |
| GPU PDSCH/PUSCH/PUCCH path starts and runs without NIC registration | ✅ Complete |
| PUCCH format 0/1 UCI indications decode correctly in bridge | ✅ Complete |
| PUSCH CRC/UCI indications flow back to gnb | ✅ Complete |
| PUCCH format 2/3/4 CSI/UCI observed in this OCUDU test config | ⚪ Not observed yet |
| CRC indications match scheduled PUSCH grants in current reruns | ✅ Complete |
| Sustained stable operation (10s / 20s / 60s reruns) | ✅ Complete |
| Slot indication continuity across SFN wrap | ✅ Complete |
| No-NIC long-run Aerial `Err.ind` / aggregate exhaustion in current reruns | ✅ Complete |
| OCUDU split-6 slot-indication queue saturation in current reruns | ✅ Complete |

### Current Notes

The latest validated reruns on GB10 show:

- no slot jumps
- no `DL task queue is full` warnings
- no Aerial `Err.indication`
- no bridge-side CRC/UCI mismatches
- continuous `CRC.indication` and `UCI.indication` flow through 60-second no-NIC runs

The key fixes that enabled this state were:

1. Aerial no-FH/no-NIC runtime guards and larger DL aggregate pools
2. bridge fixes for non-`SCF_FAPI_10_04` parsing plus correct PUCCH/PUSCH packing
3. OCUDU split-6 HyperSFN extension based on the received slot stream instead of host wall clock
4. a larger OCUDU split-6 slot-indication executor queue to absorb bursty no-NIC timing

Formal radio-quality validation is still pending. This repo now demonstrates stable FAPI integration and sustained GPU L1 operation without a NIC, but it does not yet publish a measured BLER report.

## Validation Snapshot (2026-04-10)

Latest characterization run:

- environment: `aerial-test` on GB10, no NIC, OCUDU `release_26_04`, Aerial `25.3.2`
- command: `/opt/aerial_bridge/scripts/run_integration_test.sh 60`
- report tool: [`characterize_integration.py`](/home/deepsig/src/ocudu-aerial/scripts/characterize_integration.py)

Runtime health from the latest `60s` rerun:

| Metric | Value |
|--------|-------|
| slot jumps | `0` |
| DL queue full warnings | `0` |
| forced NACK warnings | `0` |
| discarded UCI PDUs | `0` |
| Aerial `Err.ind` | `0` |
| aggregate exhaustion | `0` |
| bridge CRC mismatch logs | `0` |
| bridge UCI mismatch logs | `0` |

Bridge summary from that same rerun:

| Counter | Value |
|---------|-------|
| `tx_data_reqs` | `75330` |
| `tx_data_bytes` | `2389865056` |
| `crc_total` | `32283` |
| `crc_ok` | `0` |
| `crc_fail` | `32283` |
| `rx_data_pdus` | `32283` |
| `rx_data_bytes` | `0` |
| `uci_total` | `32284` |
| `uci_pusch` | `32283` |
| `uci_pucch01` | `1` |
| `uci_pucch234` | `0` |
| `rach_total` | `0` |
| `srs_total` | `0` |

Derived characterization metrics:

| Metric | Value |
|--------|-------|
| DL scheduled throughput | `318.649 Mbps` |
| raw UL CRC pass count | `0 / 32283` |
| raw UL BLER | `100%` |
| UL RX_DATA payload bytes | `0` |

Important interpretation:

- The `60s` run is a stable FAPI/runtime pass.
- The raw UL BLER is **not** a valid real-radio quality result in this setup.
- OCUDU test mode drives MAC scheduling without a real UE/RF waveform source feeding Aerial's UL decoder.
- In this no-UE/no-RF mode, raw Aerial `CRC.indication` failures and `RX_DATA` payload length `0` are expected.
- So this repo currently proves stable no-NIC GPU L1 integration, not true OTA-quality UL decode performance.

## Validation Coverage

### FAPI Message Coverage

| Message | Bridge Support | Current Validation State |
|---------|----------------|--------------------------|
| `PARAM.response` | Implemented | covered in bridge/runtime startup |
| `CONFIG.request/response` | Implemented | validated |
| `START.request` | Implemented | validated |
| `STOP.indication` | Implemented | basic handling present |
| `ERROR.indication` | Implemented | basic handling present |
| `DL_TTI.request` | Implemented | validated in 60s rerun |
| `UL_TTI.request` | Implemented | validated in 60s rerun |
| `UL_DCI.request` | Implemented | exercised in current config |
| `TX_DATA.request` | Implemented | validated in 60s rerun |
| `SLOT.indication` | Implemented | validated, no slot jumps in current reruns |
| `CRC.indication` | Implemented | validated for transport/association |
| `RX_DATA.indication` | Implemented | validated for transport; payload remains zero in no-UE setup |
| `UCI.indication` | Implemented | validated for PUSCH and PUCCH F0/F1 |
| `RACH.indication` | Implemented | parser/forwarding present, not exercised in latest reruns |
| `SRS.indication` | Implemented | parser/forwarding present, not exercised in latest reruns |

### Channel Coverage

| Channel | Current State |
|---------|---------------|
| `SSB/PBCH` | configured in L1, not explicitly signoff-tested in latest rerun |
| `PDCCH` | configured and active as part of stable scheduling |
| `PDSCH` | active and stable in current reruns |
| `PUSCH` | active and stable at FAPI/runtime level; raw PHY quality not validated in no-UE setup |
| `PUCCH F0/F1` | observed and validated |
| `PUCCH F2/F3/F4` | not observed in latest test-mode config |
| `PRACH` | configured, not exercised in latest reruns |
| `SRS` | configured, not exercised in latest reruns |

### What Is Validated vs. Still Open

Validated now:

1. OCUDU L2/L3 can drive Aerial GPU L1 over nvIPC without a NIC.
2. The no-NIC integration path is stable in current `10s`, `20s`, and `60s` reruns.
3. Bridge serialization/parsing is correct enough for sustained PDSCH, PUSCH, PUCCH F0/F1, CRC, UCI, and RX_DATA message flow.
4. OCUDU split-6 timing and queueing fixes removed the earlier slot-jump and queue-saturation failures.

Still open:

1. True radio-quality characterization needs a real UL waveform source or a real loopback PHY source.
2. `PUCCH F2/F3/F4`, `PRACH`, and `SRS` need explicit exercising/signoff in targeted configs.
3. The bridge still emits a fixed n78/TDD/100 MHz config profile instead of deriving the full FAPI config dynamically from OCUDU runtime config.

## Version Matrix

| Component | Version | Notes |
|-----------|---------|-------|
| OCUDU | `release_26_04` | Full L2/L3 with test mode UE |
| NVIDIA Aerial | `25.3.2` | GPU L1 with SM_120 PTX |
| Base Image | `nvcr.io/nvidia/aerial/aerial-cuda-accelerated-ran:25-3-cubb` | NGC |
| CUDA | 12.9.1 | SM_120 (Blackwell) support |
| GCC | 12.3.0 | ARM aarch64 |
| Target GPU | NVIDIA GB10 (DGX Spark) | 48 SMs, SM_120, unified LPDDR5X |

## Prerequisites

- Docker with [NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/install-guide.html)
- NVIDIA GPU (tested on DGX Spark GB10)
- Access to NVIDIA NGC registry (`nvcr.io`)
- Hugepages: `echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages`

## Building

```bash
# Build Docker image (~30 min, CUDA SM_120 compilation is slow on ARM)
make build
# or directly:
docker build --network host -t ocudu-aerial:latest .
```

The Dockerfile:
1. Starts from NGC Aerial base image (CUDA 12.9, DOCA 3.0, DPDK)
2. Clones and builds Aerial 25.3.2 with SM_120 CUDA architecture
3. Applies 8 source patches for DGX Spark GB10 compatibility
4. Clones and builds OCUDU release_26_04
5. Builds the FAPI bridge plugin
6. Re-links gnb with the bridge plugin (split_6 mode)

## Running

### Test 1: bridge_test (standalone FAPI validation)

Validates nvIPC transport and FAPI CONFIG/START/SLOT_IND flow:

```bash
docker run -d --name aerial-test --gpus all --privileged \
  --shm-size=16g --network host ocudu-aerial:latest sleep infinity

docker exec -it aerial-test bash

# Inside container:
mkdir -p /dev/hugepages && mount -t hugetlbfs nodev /dev/hugepages

# Adjust SM counts for GB10 (48 SMs)
CONFIG=/opt/nvidia/aerial/cuPHY-CP/cuphycontroller/config/cuphycontroller_P5G_GH.yaml
sed -i 's/mps_sm_pusch: 100/mps_sm_pusch: 12/' $CONFIG
sed -i 's/mps_sm_ul_order: 20/mps_sm_ul_order: 4/' $CONFIG
sed -i 's/mps_sm_pdsch: 102/mps_sm_pdsch: 12/' $CONFIG
sed -i 's/mps_sm_pdcch: 10/mps_sm_pdcch: 4/' $CONFIG
sed -i 's/mps_sm_gpu_comms: 16/mps_sm_gpu_comms: 4/' $CONFIG
sed -i 's/mps_sm_srs: 16/mps_sm_srs: 4/' $CONFIG

# Start L1
cd /opt/nvidia/aerial/build
./cuPHY-CP/cuphycontroller/examples/cuphycontroller_scf P5G_GH &

# Immediately start bridge_test (L1 exits if no client connects quickly)
sleep 3
timeout 30 /opt/aerial_bridge/build/bridge_test
```

Expected output:
```
CONFIG_RESPONSE: error_code=0x00 (OK)
SLOT_IND #1: SFN=0 Slot=3
SLOT_IND #2: SFN=0 Slot=4
...
```

### Test 2: Full gnb + GPU L1

Runs OCUDU's MAC scheduler with test mode UE driving GPU PDSCH/PUSCH:

```bash
docker exec -it aerial-test bash

# Setup (same SM adjustments as above, plus hugepages)

# Create gnb config
cat > /tmp/gnb_test.yml << 'YAML'
gnb_id: 411
ran_node_name: ocudu_aerial_gb10
cu_cp:
  amf:
    no_core: true
    addr: 127.0.0.1
    bind_addr: 127.0.0.1
cell_cfg:
  dl_arfcn: 640000
  band: 78
  channel_bandwidth_MHz: 100
  common_scs: 30
  plmn: "00101"
  tac: 7
  pci: 1
  nof_antennas_dl: 2
  nof_antennas_ul: 2
test_mode:
  test_ue:
    rnti: 0x44
    nof_ues: 1
    pdsch_active: true
    pusch_active: true
    auto_ack_indication_delay: 1
aerial:
  nvipc_prefix: nvipc
  numerology: 1
  rx_priority: 90
log:
  all_level: warning
  fapi_level: info
YAML

# Start L1
cd /opt/nvidia/aerial/build
./cuPHY-CP/cuphycontroller/examples/cuphycontroller_scf P5G_GH &

# Start gnb immediately
sleep 3
cd /opt/ocudu/build/apps/gnb
./gnb -c /tmp/gnb_test.yml aerial --nvipc_prefix nvipc --rx_priority 90 --numerology 1
```

Expected:
- `CONFIG_RESPONSE: error_code=0x00 (OK — cell configured)`
- `==== gNB started ===`
- `UCI.indication sfn=X slot=Y num_ucis=1`
- PDSCH PDU debug showing `tb_size`, `Qm`, `mcs`, `rb`, `sym` values

Convenience wrapper:

```bash
/opt/aerial_bridge/scripts/run_integration_test.sh 30
```

This script starts L1 via `/usr/local/bin/start_l1.sh` and then launches `gnb`
with the bridge plugin CLI subcommand.

### Test 3: Experimental Fronthaul / O-RU Bring-Up Example on GB10

This repository is validated today only for the **no-NIC nvIPC/FAPI path**. The
following example is provided as a starting point for lab bring-up with a real
NIC and O-RU, but it has **not yet been validated in this repository with a
radio in the loop**.

Important caveats before using it:

- This repo's validated path is `AERIAL_SKIP_NIC_REG=1` with fronthaul disabled.
- NVIDIA's documented O-RU bring-up flow assumes a fronthaul-capable Aerial
  host configuration and vendor-specific YAML, for example
  `cuphycontroller_P5G_FXN_GH.yaml` in current Aerial OTA docs.
- GB10/DGX Spark does **not** provide the same GPUDirect RDMA profile as the
  Grace Hopper systems NVIDIA documents for full fronthaul deployment, so treat
  the procedure below as an experimental example only.
- You must provide the real NIC PCIe address, O-RU destination MAC address, VLAN,
  and any vendor-specific RU settings required by your lab.

Reference source for the shape of this configuration:

- NVIDIA Aerial RAN CoLab OTA validation guide:
  <https://docs.nvidia.com/aerial/aerial-ran-colab-ota/current/text/installation_guide/validate_setup.html>
- NVIDIA cuBB quickstart note for O-RU-specific YAML selection:
  <https://docs.nvidia.com/aerial/cuda-accelerated-ran/25-2/aerial_cubb/cubb_quickstart/running_cubb-end-to-end.html>

Example flow inside the container:

```bash
docker exec -it aerial-test bash

mkdir -p /dev/hugepages && mount -t hugetlbfs nodev /dev/hugepages

CONFIG_DIR=/opt/nvidia/aerial/cuPHY-CP/cuphycontroller/config
PROFILE=P5G_GH

# If your Aerial build includes a vendor-specific FH/O-RU profile such as
# cuphycontroller_P5G_FXN_GH.yaml, start from that instead of P5G_GH.
CONFIG=$CONFIG_DIR/cuphycontroller_${PROFILE}.yaml

# Replace these with your actual lab values.
sed -i 's/ nic:.*/ nic: 0000:b5:00.0/' "$CONFIG"
sed -i 's/ dst_mac_addr:.*/ dst_mac_addr: 6c:ad:ad:00:02:02/' "$CONFIG"
sed -i 's/ vlan:.*/ vlan: 2/' "$CONFIG"

# Real FH/NIC attempt: do not skip NIC registration.
export AERIAL_SKIP_NIC_REG=0

cd /opt/nvidia/aerial/build
./cuPHY-CP/cuphycontroller/examples/cuphycontroller_scf "$PROFILE"
```

To pair this with OCUDU gNB in the same container after L1 is up:

```bash
cd /opt/ocudu/build/apps/gnb
./gnb -c /tmp/gnb_test.yml aerial --nvipc_prefix nvipc --rx_priority 90 --numerology 1
```

Recommended additional lab prerequisites before trying this path:

- NIC and switch configured for the expected O-RAN fronthaul MTU/VLANs
- PTP / PHC synchronization configured for the FH NIC
- hugepages, VFIO/IOMMU, and DPDK prerequisites satisfied for your NIC
- RU-specific YAML reviewed against the exact radio vendor and band profile

If you want to productize this path in this repo, the next step should be a
dedicated radio-in-loop validation section with the exact NIC, RU, timing, and
YAML values that were actually tested on hardware.

## DGX Spark GB10 Patches

The GB10 Blackwell desktop GPU lacks several features that Aerial expects on data-center GPUs. These patches are applied automatically during Docker build:

### 1. MPS Single Shared Context (`mps_single_ctx_fix.py`)

**Problem**: `cuCtxCreate_v3` with SM affinity fails (err=224). Creating 8 separate contexts via `cuCtxCreate_v2` causes cross-context "invalid resource handle" errors.

**Fix**: Use `cuDevicePrimaryCtxRetain` for the shared fallback context. This ensures all CUDA APIs (driver and runtime) operate on the same primary context. Eliminates ALL "invalid resource handle" errors during GPU warmup.

**Key insight**: `cuCtxCreate_v2` creates a non-primary context. `cudaStreamCreateWithPriority` (runtime API) creates streams in the PRIMARY context, not the driver API context. Using `cuDevicePrimaryCtxRetain` eliminates this mismatch.

### 2. gpinned_buffer cudaHostAlloc Fallback (`gpinned_buffer_fix.py`)

**Problem**: `gdr_pin_buffer` calls `nvidia_p2p_get_pages` which returns EINVAL on GB10 (no GPUDirect RDMA — unified LPDDR5X memory, no dedicated HBM).

**Fix**: When `gdr_pin_buffer` fails, fall back to `cudaHostAlloc(cudaHostAllocMapped | cudaHostAllocPortable)` + `cudaHostGetDevicePointer()`. On GB10, this is zero-copy since CPU and GPU share the same physical memory.

### 3. NIC Registration Non-Fatal (`nic_registration_fix.py`)

**Problem**: DOCA can't register GPU memory with the NIC via RDMA (no GPUDirect RDMA on GB10). NIC registration crashes L1.

**Fix**: Make NIC registration failure non-fatal. L1 continues without O-RAN fronthaul, serving FAPI via nvIPC only.

### 4. Fronthaul TX Guards (`fh_tx_nonfatal_fix.py`)

**Problem**: DL/UL workers crash when trying to send data to non-existent fronthaul peers.

**Fix**: Guard all FhProxy methods (`prepareUPlanePackets`, `sendCPlane`, `UserPlaneSendPackets`, `UserPlaneSendPacketsGpuComm`, `UpdateTxMetricsGpuComm`, etc.) with `peer_map.empty()` early return for FAPI-only operation.

### 5. SM_120 CUDA Architecture

**Problem**: Aerial only compiles CUDA kernels for SM_80 (A100) and SM_90 (H100). GB10 needs SM_120.

**Fix**: Add `120` to `CMAKE_CUDA_ARCHITECTURES`. Uses PTX (not native SASS) due to some test targets failing with `120-real`. Only essential targets (`cuphy`, `cuphy_ldpc`, `cuphydriver`, `cuphycontroller_scf`) are rebuilt with SM_120.

### 6. Other Patches

- **PTI BAR mmap** (`cuphy_pti_fix.py`): Software timing when PCI BAR mmap blocked
- **GDRcopy non-fatal** (`gdrcopy_fix.py`): `gdr_open` failure sets `init_gdr=false`
- **Standalone driver proxy**: `l1_staticBFWConfigured` returns 0 instead of -1
- **DPDK/toolchain**: `-mcpu=neoverse-n1` instead of `-march=native`

## FAPI Bridge Details

### CONFIG_REQUEST TLVs (30 total)

**Critical format note**: Aerial L1 is built WITHOUT `SCF_FAPI_10_04`. TLV headers use **`uint16_t length`** (2 bytes), NOT `uint32_t` (4 bytes).

| Group | TLVs | Notes |
|-------|------|-------|
| Carrier (9) | DL/UL_BANDWIDTH, DL/UL_FREQ, NUM_TX/RX_ANT, PHY_CELL_ID, DUPLEX, SCS | n78 TDD 100MHz 30kHz 2T2R |
| Grid (2) | DL/UL_GRID_SIZE | 5×uint16_t per numerology, mu=1 → 273 PRBs |
| SSB (7) | PBCH_POWER, OFFSET_POINT_A, PERIOD, SUBCARRIER_OFFSET, MIB, MASK×2 | 20ms period |
| PRACH (10) | SEQ_LEN, SCS, RESTRICTED_SET, CONFIG_INDEX, NUM_FD_OCC, ROOT_SEQ, NUM_ROOT, K1, ZERO_CORR, SSB_PER_RACH | Per-FD-occasion: ROOT_SEQ_INDEX increments fd_index |
| TDD (2) | TDD_PERIOD (6=5ms), SLOT_CONFIG (140 bytes, DDDSU×2) | Single TLV with all slots |

### PDSCH Codeword Serialization

Must match `scf_fapi_pdsch_codeword_t` field order:
```
target_code_rate(2) + qam_mod_order(1) + mcs_index(1) + mcs_table(1) + rv_index(1) + tb_size(4)
```

`target_code_rate` is serialized from the selected MCS table entry so the bridge matches the intended MCS-table semantics seen by Aerial.

### PUSCH PDU

Must include `pusch_identity` (2 bytes) between `ul_dmrs_scrambling_id` and `scid` to match `scf_fapi_pusch_pdu_t`.

### TX_DATA PDU

- `pdu_len` = TB data size (L1 without SCF_FAPI_10_04 uses `pdu_len` as TB size)
- TLV length is `uint16_t` (2 bytes)
- No `cw_index` field

## GPUDirect RDMA on GB10

**GPUDirect RDMA is architecturally impossible on DGX Spark.** The GB10 uses unified LPDDR5X memory shared between Grace CPU and Blackwell GPU via NVLink-C2C — there is no dedicated HBM. NVIDIA officially confirms this in the [DGX Spark Porting Guide](https://docs.nvidia.com/dgx/dgx-spark-porting-guide/porting/cuda.html).

Affected: `nvidia_p2p_get_pages`, `gdr_pin_buffer`, `gdr_pin_buffer_v2`, `nvidia-peermem`, `dma-buf`, DOCA GPU RxQ.

Workaround: `cudaHostAlloc(Mapped|Portable)` provides both host and device pointers with zero-copy on unified memory.

## Current Open Gaps

The current codebase demonstrates stable no-NIC FAPI integration, but it is not yet a full production-grade validation package. The main remaining gaps are:

1. **Radio-quality characterization**
   The latest `60s` characterization run is a runtime/FAPI pass, but it is not a meaningful raw UL PHY-quality pass because there is no real UE/RF waveform source. Real BLER and throughput signoff still need either a true UL loopback waveform source or an external UE/RF path.

2. **Explicit channel signoff beyond the current test-mode mix**
   `PUCCH F2/F3/F4`, `PRACH`, and `SRS` have bridge support, but they are not exercised in the latest default no-NIC reruns and therefore do not yet have explicit signoff.

3. **Dynamic config derivation**
   The bridge still emits a fixed n78/TDD/100 MHz profile instead of deriving the full FAPI config from OCUDU runtime config. The current validated path is therefore configuration-specific.

4. **Longer soak and automated reporting**
   The repo now includes characterization reporting, but longer soak runs and automated pass/fail thresholds for coverage and quality are still to be formalized.

## Container Layout

| Path | Contents |
|------|----------|
| `/opt/nvidia/aerial/` | Aerial source + build |
| `/opt/nvidia/aerial/build/cuPHY-CP/cuphycontroller/examples/cuphycontroller_scf` | Full GPU L1 binary |
| `/opt/ocudu/` | OCUDU source + build |
| `/opt/ocudu/build/apps/gnb/gnb` | gNB binary (linked with bridge) |
| `/opt/aerial_bridge/` | Bridge plugin source + build |
| `/opt/aerial_bridge/build/bridge_test` | Standalone FAPI test |
| `/usr/local/bin/start_l1.sh` | L1 startup helper (hugepages + SM counts) |

## Repository Structure

```
├── Dockerfile                          # Multi-stage build
├── LICENSE                             # BSD-3-Clause-Clear license
├── Makefile                            # build/run/shell/stop/clean
├── README.md                           # This file
├── aerial_bridge/
│   ├── CMakeLists.txt                  # Bridge build system
│   ├── DESIGN.md                       # Architecture document
│   ├── config/nvipc_secondary.yaml     # nvIPC config
│   ├── include/aerial_bridge/          # Public headers
│   ├── patches/
│   │   ├── cuphy_pti_fix.py            # PTI BAR mmap fix
│   │   ├── gdrcopy_fix.py             # GDRcopy non-fatal
│   │   ├── gpu_comm_standalone_fix.py  # No-NIC/FH-safe Aerial runtime patch
│   │   ├── gpinned_buffer_fix.py      # cudaHostAlloc fallback
│   │   ├── mps_single_ctx_fix.py      # Primary context MPS fix
│   │   ├── nic_registration_fix.py    # NIC non-fatal
│   │   └── ocudu_executor_queue_fix.py # OCUDU queue-depth fix
│   │
│   └── src/
│       ├── aerial_fapi_adaptor.cpp     # P5/P7 FAPI gateway
│       ├── aerial_split6_plugin.cpp    # OCUDU plugin factory
│       ├── bridge_test.cpp             # Standalone test
│       ├── gdr_stub.cpp                # GDRcopy stub library
│       ├── msg_translator.cpp          # FAPI message serialization
│       ├── nvipc_transport.cpp         # nvIPC lifecycle
│       └── rx_thread.cpp              # RX indication dispatch
├── ocudu_patches/                      # Vendored OCUDU fixes used in image build
│   ├── slot_point_extender_adaptor.cpp
│   └── slot_point_extender_adaptor.h
└── scripts/
    ├── characterize_integration.py     # Metrics/coverage report generator
    ├── make_loopback_config.py         # Loopback config helper
    ├── run_integration_test.sh         # Full integration test
    └── start_l1.sh                     # L1 startup helper
```
