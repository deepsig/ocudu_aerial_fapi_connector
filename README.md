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

## Current Status (2026-04-28)

This branch now targets NVIDIA Aerial `26.1.0` by default. The validated local
path remains the **external SCF/FAPI over nvIPC path with fronthaul/NIC
registration disabled** on DGX Spark GB10.

What is currently green:

| Milestone | Status |
|-----------|--------|
| Docker build (Aerial + OCUDU + bridge) | ✅ Complete |
| Aerial `26.1.0` source build and OCUDU relink | ✅ Complete |
| `bridge_test` external FAPI smoke (`CONFIG/START/SLOT_IND`) | ✅ Complete |
| Full OCUDU `gnb` + Aerial L1 startup on `26.1.0` | ✅ Complete |
| Sustained DL scheduling (`DL_TTI` + `TX_DATA`) | ✅ Complete |
| Sustained UL scheduling (`UL_TTI` + `CRC/UCI`) | ✅ Complete |
| UL structural sanity (`schedule -> CRC/UCI` coherence) | ✅ Complete |
| Waveform asset preflight for `nrSim` / `90604` | ✅ Complete |

What remains open or unvalidated:

| Gap | Current State |
|-----|---------------|
| True UL payload correctness | ⚪ not validated in no-UE/no-RF mode |
| PUCCH F2/F3/F4 explicit signoff | ⚪ not observed in current test mode |
| WNC / real O-RU radio-in-loop path | ⚪ documented, not validated here |

Two runtime requirements matter for any local rerun:

1. start the container with `--shm-size=16g`
2. use the repo wrappers (`/usr/local/bin/start_l1.sh`,
   `run_integration_test.sh`, `run_validation_suite.sh`) instead of raw
   upstream `cuphycontroller_scf` defaults

The most important consequence of the `26.1.0` upgrade is that upstream added
more standalone/loopback support, but that did **not** replace this repo's
external OCUDU-facing FAPI path. The no-NIC integration path still needs
repo-local runtime guards and GB10-specific launch adjustments.

See [`CHANGELOG.md`](/home/deepsig/src/ocudu-aerial/CHANGELOG.md) for the
patch audit and upgrade summary.

## Validation Snapshot (2026-04-28)

Latest current-tree characterization run:

- base image/runtime: Aerial `26.1.0` (`26-1-cubb`) with current-tree wrapper scripts
- host/container requirements:
  - `--gpus all --privileged --network host --shm-size=16g`
  - `-v /dev/hugepages:/dev/hugepages`
- suite:
  `/opt/aerial_bridge/scripts/run_validation_suite.sh 20`
- report tool:
  [`characterize_integration.py`](/home/deepsig/src/ocudu-aerial/scripts/characterize_integration.py)

`bridge_test` result in that run:

- `CONFIG_RESPONSE: error_code=0x00`
- `START_REQUEST sent`
- continuous `SLOT_IND` flow observed (`#6000` reached in the captured smoke log)

Full OCUDU integration result from that same run:

| Metric | Value |
|--------|-------|
| overall runtime health | `PASS` |
| slot jumps | `0` |
| DL queue full warnings | `0` |
| forced NACK warnings | `0` |
| Aerial `Err.ind` | `0` |
| aggregate exhaustion | `0` |
| L1 tick error alerts | `0` |
| DL scheduled throughput | `281.952 Mbps` |
| DL `TX_DATA` bytes | `704879936` |
| UL CRC indications | `9527` |
| UL UCI indications | `9528` (`9527` PUSCH + `1` PUCCH F0/F1) |
| UL structural sanity | `PASS` |
| raw UL CRC pass count | `0 / 9527` |
| UL RX_DATA payload bytes | `0` |

Current interpretation:

- This branch proves that OCUDU L2/L3 still drives Aerial `26.1.0` L1 over
  external nvIPC/FAPI on GB10.
- `bridge_test` is a real pass, not a dry-run.
- The full OCUDU path now passes the current local 20s suite run with the
  tuned no-NIC launch profile in this repo.
- The most important launch deltas versus the earlier `26.1.0` partial-pass
  runs are the no-NIC timer slack, separated L2A message-thread core, and a
  pinned OCUDU bridge RX thread.
- `ul_structural_sanity=PASS` means OCUDU scheduled UL grants and received
  coherent CRC/UCI back for those exact grants.
- `raw_ul_bler_pct=100%` is expected in this no-UE/no-RF mode and is **not**
  an OTA radio-quality result.
- True UL payload correctness still needs a waveform source or a real RU/UE
  path. The optional `nrSim` waveform flow in this repo is a sibling PHY check,
  not inline OCUDU waveform injection.

## Validation Coverage

### FAPI Message Coverage

| Message | Bridge Support | Current Validation State |
|---------|----------------|--------------------------|
| `PARAM.response` | Implemented | covered in bridge/runtime startup |
| `CONFIG.request/response` | Implemented | validated |
| `START.request` | Implemented | validated |
| `STOP.indication` | Implemented | basic handling present |
| `ERROR.indication` | Implemented | basic handling present |
| `DL_TTI.request` | Implemented | validated in current `26.1.0` suite run |
| `UL_TTI.request` | Implemented | validated in current `26.1.0` suite run |
| `UL_DCI.request` | Implemented | exercised in current config |
| `TX_DATA.request` | Implemented | validated in current `26.1.0` suite run |
| `SLOT.indication` | Implemented | validated; no slot jumps in the current suite run |
| `CRC.indication` | Implemented | validated for transport/association |
| `RX_DATA.indication` | Implemented | validated for transport; payload remains zero in no-UE setup |
| `UCI.indication` | Implemented | validated for PUSCH and PUCCH F0/F1 |
| `RACH.indication` | Implemented | parser/forwarding present; PRACH scheduling observed in current suite run |
| `SRS.indication` | Implemented | parser/forwarding present; not exercised in current suite run |

### Channel Coverage

| Channel | Current State |
|---------|---------------|
| `SSB/PBCH` | configured in L1; not explicitly payload-signoff tested in the current suite run |
| `PDCCH` | configured and active as part of current scheduling |
| `PDSCH` | active in the current suite run; local 20s runtime health pass achieved |
| `PUSCH` | active at FAPI/runtime level; raw PHY quality not validated in no-UE setup |
| `PUCCH F0/F1` | observed and validated |
| `PUCCH F2/F3/F4` | not observed in the current test-mode config |
| `PRACH` | configured and scheduled in the current suite run |
| `SRS` | configured, not exercised in the current suite run |

### What Is Validated vs. Still Open

Validated now:

1. OCUDU L2/L3 can drive Aerial GPU L1 over nvIPC without a NIC.
2. The `bridge_test` external FAPI smoke path is working on current `26.1.0`.
3. Bridge serialization/parsing is correct enough for sustained PDSCH, PUSCH, PUCCH F0/F1, CRC, UCI, and RX_DATA indication flow.
4. OCUDU split-6 timing and queueing fixes removed the earlier slot-jump and queue-saturation failures.
5. The wrapper scripts now encode the GB10 runtime requirements that matter in practice: SM clamps, no-NIC launch mode, and the large `/dev/shm` requirement.

Still open:

1. Long-duration soak stability on the no-NIC `26.1.0` path is still open beyond the current short local suite run.
2. True inline OCUDU + waveform characterization still needs a real UL waveform source or a fronthaul-backed same-host RU path. The repo now includes a sibling `nrSim` waveform validator for Aerial PHY signoff.
3. `PUCCH F2/F3/F4` and `SRS` still need explicit exercising/signoff in targeted configs.
4. WNC / real O-RU validation remains a separate radio-in-loop task.
5. The bridge still emits a fixed n78/TDD/100 MHz config profile instead of deriving the full FAPI config dynamically from OCUDU runtime config.

## Version Matrix

| Component | Version | Notes |
|-----------|---------|-------|
| OCUDU | `release_26_04` | Full L2/L3 with test mode UE |
| NVIDIA Aerial | `26.1.0` | Current default build target; last archived rerun above was on `25.3.2` |
| Base Image | `nvcr.io/nvidia/aerial/aerial-cuda-accelerated-ran:26-1-cubb` | NGC |
| CUDA | 13.1.1 | Per NVIDIA `26-1` software manifest |
| GCC | 12.3.0 | ARM aarch64 |
| Target GPU | NVIDIA GB10 (DGX Spark) | 48 SMs, SM_120, unified LPDDR5X |

## Prerequisites

- Docker with [NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/install-guide.html)
- NVIDIA GPU (tested on DGX Spark GB10)
- Access to NVIDIA NGC registry (`nvcr.io`)
- Hugepages: `echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages`
- Large container shared memory: run test containers with `--shm-size=16g`

## Building

```bash
# Build Docker image (~30 min, CUDA SM_120 compilation is slow on ARM)
make build
# or directly:
docker build --network host -t ocudu-aerial:latest .
```

The Dockerfile:
1. Starts from NGC Aerial base image (CUDA 13.1, DOCA 3.2, DPDK)
2. Clones and builds Aerial 26.1.0 with SM_120 CUDA architecture
3. Applies the remaining repo-local DGX Spark / no-NIC patches still needed on top of upstream 26.1.0
4. Clones and builds OCUDU release_26_04
5. Builds the FAPI bridge plugin
6. Re-links gnb with the bridge plugin (split_6 mode)

## Running

### Test 1: bridge_test (standalone FAPI validation)

Validates external nvIPC/FAPI transport and `CONFIG/START/SLOT_IND` flow:

```bash
docker run -d --name aerial-test --gpus all --privileged \
  --shm-size=16g --network host \
  -v /dev/hugepages:/dev/hugepages \
  ocudu-aerial:latest sleep infinity

docker exec -it aerial-test bash

/usr/local/bin/start_l1.sh > /tmp/l1.log 2>&1 &
sleep 12
timeout 20 /opt/aerial_bridge/build/bridge_test
```

Expected output:
```
CONFIG_RESPONSE: error_code=0x00 (OK)
SLOT_IND #1: SFN=0 Slot=3
SLOT_IND #2: SFN=0 Slot=4
...
```

### Test 2: Full gnb + GPU L1

Runs OCUDU test mode UE against Aerial L1 through the bridge:

```bash
docker exec -it aerial-test bash

AERIAL_LOG_DIR=/tmp/aerial_integration \
  /opt/aerial_bridge/scripts/run_integration_test.sh 20
```

This wrapper:

1. starts Aerial via `/usr/local/bin/start_l1.sh`
2. applies the GB10 launch adjustments required for the validated no-NIC path
3. launches `gnb` in OCUDU test mode
4. writes logs under `${AERIAL_LOG_DIR:-/tmp/aerial_integration}`
5. ends by running [`characterize_integration.py`](/home/deepsig/src/ocudu-aerial/scripts/characterize_integration.py)

Current validated local defaults baked into the wrappers:

- `AERIAL_SKIP_NIC_REG=1`
- `AERIAL_ALLOWED_FAPI_LATENCY=16`
- `AERIAL_ALLOWED_TICK_ERROR_US=400`
- `AERIAL_TIMER_THREAD_WAKEUP_THRESHOLD_NS=400000`
- `AERIAL_MSG_THREAD_CPU=12`
- `AERIAL_RX_CPU_CORE=13`

These can all be overridden through the environment when you want to compare
timing behavior.

Convenience wrapper:

```bash
/opt/aerial_bridge/scripts/run_integration_test.sh 30
```

This script starts L1 via `/usr/local/bin/start_l1.sh` and then launches `gnb`
with the bridge plugin CLI subcommand. It now ends by running
`characterize_integration.py` automatically.

Full coverage wrapper:

```bash
/opt/aerial_bridge/scripts/run_validation_suite.sh 30
```

This runs both `bridge_test` and the full `gnb` integration path, then fails if
either the external FAPI smoke test or the `ul_structural_sanity` checks fail.
On the current tuned no-NIC `26.1.0` path, the latest local 20s suite rerun
passes end-to-end.

### Test 3: Waveform-backed nrSim PHY validation

Runs NVIDIA's `dlc_test_bench` with real `nrSim` test vectors against the same Aerial build used by the OCUDU integration path:

```bash
# Inside container:
export AERIAL_TV_SRC=/mnt/cicd_tvs/develop/GPU_test_input
/opt/aerial_bridge/scripts/run_waveform_validation.sh 90604
```

Or from the host:

```bash
make validate-waveform CONTAINER_NAME=aerial-test PATTERN=90604 \
  TV_SRC=/mnt/cicd_tvs/develop/GPU_test_input
```

What this does:

1. stages a scratch Aerial SDK root under `/tmp/aerial_waveform_validation`
2. copies/generates the pattern-specific `nrSim` waveform assets
3. regenerates controller/RU-emulator/testMAC configs with `auto_AllConfig.py`
4. runs upstream `dlc_test_bench` against the current Aerial build

Important limitation:

- This validates waveform-backed Aerial PHY behavior and real CRC/TB outcomes on the same build, but it does not put OCUDU inline with the waveform source. Upstream external SCF/FAPI still does not ingest `tv_pusch` as live UL IQ input.

### Test 4: Experimental Fronthaul / O-RU Bring-Up Example on GB10

This repository is validated today only for the **no-NIC nvIPC/FAPI path**. The
following runbook is a practical starting point for lab bring-up with a real
NIC and O-RU such as a WNC unit, but it has **not yet been validated in this
repository with a radio in the loop**.

Short answer on expected behavior:

- **What should work in principle**:
  Aerial L1 startup, FH NIC registration, OCUDU attach over nvIPC, and normal
  OCUDU `L2/L3 -> FAPI -> Aerial L1 -> FH` operation, provided the vendor YAML,
  NIC, VLAN/MAC, PTP, and RU-specific settings are correct.
- **Why that should still be possible here**:
  the repo-local no-NIC timing tweaks are only applied when
  `AERIAL_SKIP_NIC_REG=1`. With `AERIAL_SKIP_NIC_REG=0`, the wrapper keeps the
  GB10 SM clamps but does not force the no-NIC L2A timing profile.
- **What is not signed off yet**:
  exact WNC vendor YAML selection on GB10, OTA/radio performance, long soaks,
  and any NIC/PTP/RU-specific quirks in a real lab.

Important caveats before using it:

- This repo's validated path is `AERIAL_SKIP_NIC_REG=1` with fronthaul disabled.
- NVIDIA's documented O-RU bring-up flow assumes a fronthaul-capable Aerial
  host configuration and vendor-specific YAML, for example
  `cuphycontroller_P5G_FXN_GH.yaml` in current Aerial OTA docs.
- NVIDIA's current feature documentation lists the WNC `R1220` as a tested
  n77/n78/n79 indoor O-RU, but in a `4T4R`, `100 MHz`, `30 kHz SCS`, `TDD 7.2`
  configuration rather than a special single-layer RU profile.
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
- NVIDIA feature matrix listing tested O-RUs including WNC `R1220`:
  <https://docs.nvidia.com/aerial/cuda-accelerated-ran/latest/cubb/features_and_arch/features_for_5g_gnb.html>

Recommended host-side container start:

```bash
docker run -d --name aerial-fh-test --gpus all --privileged \
  --shm-size=16g --network host \
  -v /dev/hugepages:/dev/hugepages \
  ocudu-aerial:latest sleep infinity
```

Recommended L1 bring-up flow inside the container:

```bash
docker exec -it aerial-fh-test bash

mkdir -p /dev/hugepages && mount -t hugetlbfs nodev /dev/hugepages

CONFIG_DIR=/opt/nvidia/aerial/cuPHY-CP/cuphycontroller/config
PROFILE=P5G_GH

# WNC R1220-style starting point:
# keep the standard 100 MHz n78/TDD sector profile in Aerial, and if your Aerial
# build ships a vendor-specific WNC/FH YAML, start from that instead of P5G_GH.
# The RU remains a 4T4R fronthaul endpoint; "single-layer" behavior would be
# constrained on the OCUDU traffic/scheduler side rather than by changing the RU
# profile itself.
CONFIG=$CONFIG_DIR/cuphycontroller_${PROFILE}.yaml

# Replace these with your actual lab values.
sed -i 's/ nic:.*/ nic: 0000:b5:00.0/' "$CONFIG"
sed -i 's/ dst_mac_addr:.*/ dst_mac_addr: 6c:ad:ad:00:02:02/' "$CONFIG"
sed -i 's/ vlan:.*/ vlan: 2/' "$CONFIG"

# Real FH/NIC attempt: do not skip NIC registration.
export AERIAL_SKIP_NIC_REG=0
export AERIAL_L1_PROFILE="$PROFILE"

/usr/local/bin/start_l1.sh "$PROFILE" > /tmp/l1_fh.log 2>&1
```

What to verify before starting OCUDU:

- `/tmp/l1_fh.log` does **not** contain `skipping FH NIC registration`
- `/tmp/l1_fh.log` does **not** contain `NIC registration failed`
- L1 reaches `cuPHYController initialized, L1 is ready!`
- PTP / PHC state, NIC link, VLAN, and switch-side MTU look correct

To pair this with OCUDU gNB after L1 is up, use a **real gNB config**, not the
local test harness wrapper. `run_integration_test.sh` is for the no-core
test-mode path and auto-generates a temporary local `gnb_test.yml`; it is not
the right entry point for radio-in-loop bring-up.

Example OCUDU launch shape:

```bash
cd /opt/ocudu/build/apps/gnb
./gnb -c /path/to/gnb_wnc.yml \
  aerial --nvipc_prefix nvipc --rx_cpu_core 13 --rx_priority 90 --numerology 1
```

Recommended additional lab prerequisites before trying this path:

- NIC and switch configured for the expected O-RAN fronthaul MTU/VLANs
- PTP / PHC synchronization configured for the FH NIC
- hugepages, VFIO/IOMMU, and DPDK prerequisites satisfied for your NIC
- RU-specific YAML reviewed against the exact radio vendor and band profile
- OCUDU carrier, SCS, TDD pattern, PCI, and antenna counts aligned with the
  selected Aerial FH profile

Recommended first FH-enabled bring-up sequence:

1. Bring up Aerial L1 alone with `AERIAL_SKIP_NIC_REG=0` and confirm clean NIC/FH startup in the log.
2. Start OCUDU with a matching real `gnb` config and confirm `CONFIG/START` over nvIPC.
3. Only then move to radio procedures, RU-specific tuning, and OTA/radio performance checks.

If you want to productize this path in this repo, the next step should be a
dedicated radio-in-loop validation section with the exact NIC, RU, timing, and
YAML values that were actually tested on hardware.

## DGX Spark GB10 Patches

The GB10 Blackwell desktop GPU lacks several features that Aerial expects on
data-center GPUs. The active repo-local patches and build-time adjustments
below are still applied automatically during Docker build. The only historical
patches intentionally retired on `26.1.0` are listed separately under
`Upstreamed in 26.1.0`.

### 1. MPS Single Shared Context (`mps_single_ctx_fix.py`)

**Problem**: `cuCtxCreate_v3` with SM affinity fails (err=224). Creating 8 separate contexts via `cuCtxCreate_v2` causes cross-context "invalid resource handle" errors.

**Fix**: Use `cuDevicePrimaryCtxRetain` for the shared fallback context. This ensures all CUDA APIs (driver and runtime) operate on the same primary context. Eliminates ALL "invalid resource handle" errors during GPU warmup.

**Key insight**: `cuCtxCreate_v2` creates a non-primary context. `cudaStreamCreateWithPriority` (runtime API) creates streams in the PRIMARY context, not the driver API context. Using `cuDevicePrimaryCtxRetain` eliminates this mismatch.

### 2. NIC Registration Fallback + No-Peer Runtime Guards (`nic_registration_fix.py`, `gpu_comm_standalone_fix.py`)

**Problem**: upstream `26.1.0` adds same-host RU-emulator loopback support, but the normal SCF/FAPI path still assumes NIC registration and fronthaul peers exist. On DGX Spark no-NIC runs, `registerNic()` still hard-fails and later FH/GPU-comm work still assumes peers are present.

**Fix**:

- `nic_registration_fix.py` keeps NIC registration failure non-fatal if a FH-enabled bring-up is attempted in an environment that still lacks usable GPUDirect RDMA.
- `gpu_comm_standalone_fix.py` is the larger active no-peer hardening patch. It disables FH-dependent GPU-comm paths when there are no registered peers, makes FH-only paths safe no-ops in FAPI-only runs, enlarges aggregate pools, and relaxes slot matching where needed for the current local path.

### 3. nvIPC Host-Pinning Fallback (`nvipc_host_pinning_fix.py`)

**Problem**: on GB10, Aerial's primary nvIPC SHM transport can fail `cudaHostRegister()` for the host-side SHM pools. In `26.1.0`, `l2_adapter_config_P5G_GH.yaml` also enables nvIPC PCAP settings that are not useful for the validated no-NIC path.

**Fix**: when `AERIAL_SKIP_NIC_REG=1`, tolerate SHM host-page-lock failure, treat the matching unregister failure as non-fatal, and disable the L2 adapter PCAP defaults from `start_l1.sh`.

### 4. PTI BAR mmap Fallback (`cuphy_pti_fix.py`)

**Problem**: PCI BAR mmap can still fail in this environment, which aborts PTI initialization even though hardware PTP timing is non-critical for this FAPI-only path.

**Fix**: fall back to software timing and keep the rest of L1 initialization alive.

### 5. Standalone Driver Proxy Returns

**Problem**: `l1_staticBFWConfigured()` and `l1_resetDBTStorage()` still return `-1` in upstream standalone mode.

**Fix**: force the standalone/no-FH branch to return success for the optional
standalone/loopback tooling in this repo.

### 6. Standalone Launch-Pattern Filename Fix (`standalone_filename_fix.py`)

**Problem**: upstream `26.1.0` standalone loopback startup still parses the
main cuphycontroller config path instead of `standalone_filename`, which breaks
the launch-pattern lookup for the optional loopback path.

**Fix**: patch `cuphycontroller_scf.cpp` so standalone launch-pattern parsing
uses `standalone_filename`.

This is **not** part of the validated OCUDU external FAPI path. It is retained
only for the optional Aerial standalone/loopback tooling in this repo.

### 7. SM_120 CUDA Architecture

**Problem**: Aerial only compiles CUDA kernels for SM_80 (A100) and SM_90 (H100). GB10 needs SM_120.

**Fix**: Add `120` to `CMAKE_CUDA_ARCHITECTURES`. Uses PTX (not native SASS) due to some test targets failing with `120-real`. Only essential targets (`cuphy`, `cuphy_ldpc`, `cuphydriver`, `cuphycontroller_scf`) are rebuilt with SM_120.

### 8. Upstreamed in 26.1.0

- GPUDirect RDMA capability detection is now upstream. `gpudevice.cpp` checks `cudaDevAttrGPUDirectRDMASupported` before opening GDRCopy.
- `gpinned_buffer` now has an upstream host-pinned fallback path for non-RDMA GPUs.
- Because of that, the older local `gdrcopy_fix.py` and `gpinned_buffer_fix.py` are intentionally no longer applied in this branch.
- The old `fh_tx_nonfatal_fix.py` is also intentionally gone from this branch.

### 9. Other Build-Time Adjustments

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

The current codebase demonstrates working external FAPI integration on `26.1.0`, but it is not yet a fully green production-grade validation package. The main remaining gaps are:

1. **Longer-duration no-NIC soak stability**
   The current local 20s suite run is clean, but longer soak coverage is still open.

2. **Radio-quality characterization**
   The latest current-tree run is not a meaningful raw UL PHY-quality pass because there is no real UE/RF waveform source inline with OCUDU. The repo includes an optional `nrSim` waveform-backed Aerial PHY validator, but full OCUDU + waveform signoff still needs either a fronthaul-backed same-host RU path or an external UE/RF path.

3. **Explicit channel signoff beyond the current test-mode mix**
   `PUCCH F2/F3/F4` and `SRS` have bridge support, but they are not exercised in the latest default no-NIC reruns and therefore do not yet have explicit signoff.

4. **Dynamic config derivation**
   The bridge still emits a fixed n78/TDD/100 MHz profile instead of deriving the full FAPI config from OCUDU runtime config. The current validated path is therefore configuration-specific.

5. **WNC / real O-RU validation**
   The WNC/fronthaul path is documented here, but it has not been signed off in this repo with radio hardware in the loop.

6. **Longer soak and automated reporting**
   The repo includes characterization reporting, but longer soak runs and stronger automated pass/fail thresholds are still to be formalized.

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
├── CHANGELOG.md                        # Branch-level upgrade and validation log
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
│   │   ├── gpu_comm_standalone_fix.py  # No-NIC/FH-safe Aerial runtime patch
│   │   ├── mps_single_ctx_fix.py       # Primary context MPS fix
│   │   ├── nic_registration_fix.py     # NIC non-fatal
│   │   ├── nvipc_host_pinning_fix.py   # nvIPC SHM host-page-lock fallback
│   │   ├── ocudu_executor_queue_fix.py # OCUDU queue-depth fix
│   │   └── standalone_filename_fix.py  # Aerial standalone launch-pattern fix
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
    ├── characterize_integration.py     # Metrics/coverage + UL structural report
    ├── make_loopback_config.py         # Loopback config helper
    ├── run_integration_test.sh         # Full integration test
    ├── run_validation_suite.sh         # bridge_test + integration coverage suite
    └── start_l1.sh                     # L1 startup helper
```
