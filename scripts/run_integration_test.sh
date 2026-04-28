#!/usr/bin/env bash
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ============================================================
# Aerial + OCUDU Integration Test on DGX Spark GB10
#
# Starts Aerial GPU L1 (cuphycontroller_scf) then OCUDU gnb
# with the FAPI bridge plugin and test_mode UE.
#
# Usage:
#   ./run_integration_test.sh [duration_seconds]
# ============================================================

set -euo pipefail

DURATION=${1:-30}
LOG_DIR="${AERIAL_LOG_DIR:-/tmp/aerial_integration}"
AERIAL_CHARACTERIZE="${AERIAL_CHARACTERIZE:-1}"
L1_PID=""
GNB_PID=""
mkdir -p "$LOG_DIR"

wait_for_exit() {
  local pid=$1
  local loops=${2:-20}
  local delay_s=${3:-0.1}
  local i
  for ((i = 0; i < loops; ++i)); do
    if ! kill -0 "$pid" 2>/dev/null; then
      return 0
    fi
    sleep "$delay_s"
  done
  return 1
}

if [[ "${AERIAL_L1_PROFILE:-}" == "loopback" ]]; then
  echo "ERROR: AERIAL_L1_PROFILE=loopback is Aerial standalone mode in 26.1 and does not expose the external SCF/FAPI path used by this integration test." >&2
  echo "Use the default P5G_GH profile for OCUDU integration, or run /usr/local/bin/start_l1.sh loopback separately for standalone bring-up." >&2
  exit 2
fi

echo "============================================"
echo "  Aerial + OCUDU GPU L1 Integration Test"
echo "  Duration: ${DURATION}s"
echo "============================================"
echo ""

# ── Cleanup ──────────────────────────────────────────────────
cleanup() {
  echo ""
  echo "Stopping processes..."
  if [[ -n "${GNB_PID}" ]]; then
    kill "$GNB_PID" 2>/dev/null || true
    wait_for_exit "$GNB_PID" 20 0.1 || true
  fi
  if [[ -n "${L1_PID}" ]]; then
    kill "$L1_PID" 2>/dev/null || true
  fi
  sleep 1
  [[ -n "${GNB_PID}" ]] && kill -9 "$GNB_PID" 2>/dev/null || true
  [[ -n "${L1_PID}" ]] && kill -9 "$L1_PID" 2>/dev/null || true
  echo "Logs at $LOG_DIR/"
}
trap cleanup EXIT INT TERM

# ── Mount hugepages ──────────────────────────────────────────
mkdir -p /dev/hugepages
mount -t hugetlbfs nodev /dev/hugepages 2>/dev/null || true

# ── Adjust L1 config for GB10 (48 SMs) ──────────────────────
CONFIG=/opt/nvidia/aerial/cuPHY-CP/cuphycontroller/config/cuphycontroller_P5G_GH.yaml
sed -i "s/mps_sm_pusch: 100/mps_sm_pusch: 12/" $CONFIG 2>/dev/null
sed -i "s/mps_sm_ul_order: 20/mps_sm_ul_order: 4/" $CONFIG 2>/dev/null
sed -i "s/mps_sm_pdsch: 102/mps_sm_pdsch: 12/" $CONFIG 2>/dev/null
sed -i "s/mps_sm_pdcch: 10/mps_sm_pdcch: 4/" $CONFIG 2>/dev/null
sed -i "s/mps_sm_gpu_comms: 16/mps_sm_gpu_comms: 4/" $CONFIG 2>/dev/null
sed -i "s/mps_sm_srs: 16/mps_sm_srs: 4/" $CONFIG 2>/dev/null

# ── Start Aerial L1 ─────────────────────────────────────────
echo "[1/2] Starting Aerial GPU L1..."
L1_PROFILE="${AERIAL_L1_PROFILE:-}"
if [[ -n "$L1_PROFILE" ]]; then
  /usr/local/bin/start_l1.sh "$L1_PROFILE" > "$LOG_DIR/l1.log" 2>&1 &
else
  /usr/local/bin/start_l1.sh > "$LOG_DIR/l1.log" 2>&1 &
fi
L1_PID=$!
echo "  PID: $L1_PID"

# Wait for L1 to initialize GPU and start nvIPC listener
echo "  Waiting for L1 init (~12s)..."
sleep 12

if ! kill -0 $L1_PID 2>/dev/null; then
  echo "ERROR: L1 failed to start"
  tail -10 "$LOG_DIR/l1.log"
  exit 1
fi
echo "  L1 running."

# ── Create gnb config ────────────────────────────────────────
GNB_CONFIG="$LOG_DIR/gnb_test.yml"
AUTO_ACK_DELAY="${AERIAL_TESTMODE_AUTO_ACK_DELAY:-}"
RX_CPU_CORE="${AERIAL_RX_CPU_CORE:-13}"
cat > "$GNB_CONFIG" <<YAML
# OCUDU gnb config for Aerial GPU L1 integration test
# n78 TDD, 100MHz, 30kHz SCS, 2T2R — matches P5G_GH L1 config

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
    ri: 1
    cqi: 15
    nof_ues: 1
    pdsch_active: true
    pusch_active: true
$(if [[ -n "$AUTO_ACK_DELAY" ]]; then printf '    auto_ack_indication_delay: %s\n' "$AUTO_ACK_DELAY"; fi)

log:
  filename: $LOG_DIR/gnb.log
  all_level: warning
  fapi_level: info
YAML

# ── Start OCUDU gnb ─────────────────────────────────────────
echo ""
echo "[2/2] Starting OCUDU gnb (test mode)..."
cd /opt/ocudu/build/apps/gnb
./gnb -c "$GNB_CONFIG" \
  aerial --nvipc_prefix nvipc --rx_cpu_core "$RX_CPU_CORE" --rx_priority 90 --numerology 1 \
  > "$LOG_DIR/gnb_stdout.log" 2>&1 &
GNB_PID=$!
echo "  PID: $GNB_PID"

sleep 5

if ! kill -0 $GNB_PID 2>/dev/null; then
  echo "WARNING: gnb may have exited"
  tail -20 "$LOG_DIR/gnb_stdout.log" 2>/dev/null
  tail -20 "$LOG_DIR/gnb.log" 2>/dev/null
fi

# ── Monitor ──────────────────────────────────────────────────
echo ""
echo "============================================"
echo "  Test running for ${DURATION}s"
echo "============================================"
echo ""

sleep "$DURATION"

# ── Report ───────────────────────────────────────────────────
echo ""
echo "============================================"
echo "  Test Results"
echo "============================================"

echo ""
echo "--- L1 CONFIG/START ---"
cat "$LOG_DIR/l1.log" | strings | grep -E "error_code|on_cell_start|create_cell_config|CONFIGURED" 2>/dev/null | head -5 || true

echo ""
echo "--- L1 SLOT stats ---"
SLOT_COUNT=$(cat "$LOG_DIR/l1.log" | strings | grep -c "slot_ind\|SLOT" 2>/dev/null || echo 0)
echo "  SLOT-related log entries: $SLOT_COUNT"

echo ""
echo "--- gnb FAPI ---"
grep -E "SLOT\|DL_TTI\|UL_TTI\|CONFIG\|START\|error\|bridge" "$LOG_DIR/gnb.log" 2>/dev/null | tail -10 || true
grep -E "SLOT\|bridge\|aerial" "$LOG_DIR/gnb_stdout.log" 2>/dev/null | tail -10 || true

echo ""
echo "  L1 log:  $LOG_DIR/l1.log"
echo "  gnb log: $LOG_DIR/gnb.log"
echo "============================================"

if [[ "$AERIAL_CHARACTERIZE" != "0" ]]; then
  echo ""
  python3 /opt/aerial_bridge/scripts/characterize_integration.py \
    --log-dir "$LOG_DIR" \
    --duration "$DURATION" || true
fi
