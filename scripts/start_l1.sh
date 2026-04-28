#!/usr/bin/env bash
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# Start cuphycontroller_scf L1 on DGX Spark GB10.
# Requires: --gpus all --privileged --shm-size=16g --network host.
# Default mode is the validated no-NIC nvIPC/FAPI path.
# Set AERIAL_SKIP_NIC_REG=0 to attempt real FH/NIC bring-up instead.
set -e

fail() {
  echo "ERROR: $*" >&2
  exit 1
}

apply_gb10_sm_clamps() {
  local config=$1
  sed -i "s/mps_sm_pusch: 100/mps_sm_pusch: 12/" "$config" 2>/dev/null || true
  sed -i "s/mps_sm_ul_order: 20/mps_sm_ul_order: 4/" "$config" 2>/dev/null || true
  sed -i "s/mps_sm_pdsch: 102/mps_sm_pdsch: 12/" "$config" 2>/dev/null || true
  sed -i "s/mps_sm_pdcch: 10/mps_sm_pdcch: 4/" "$config" 2>/dev/null || true
  sed -i "s/mps_sm_gpu_comms: 16/mps_sm_gpu_comms: 4/" "$config" 2>/dev/null || true
  sed -i "s/mps_sm_srs: 16/mps_sm_srs: 4/" "$config" 2>/dev/null || true
}

tune_no_nic_l2_config() {
  local config=$1
  python3 - "$config" \
    "${AERIAL_ALLOWED_FAPI_LATENCY:-16}" \
    "${AERIAL_ALLOWED_TICK_ERROR_US:-400}" \
    "${AERIAL_L2A_ALLOWED_LATENCY:-1000000}" \
    "${AERIAL_TIMER_THREAD_WAKEUP_THRESHOLD_NS:-400000}" \
    "${AERIAL_MSG_THREAD_CPU:-12}" <<'PY'
from pathlib import Path
import sys

import yaml


path = Path(sys.argv[1])
allowed_fapi_latency = int(sys.argv[2])
allowed_tick_error = int(sys.argv[3])
l2a_allowed_latency = int(sys.argv[4])
timer_thread_wakeup_threshold = int(sys.argv[5])
message_thread_cpu = int(sys.argv[6])

data = yaml.safe_load(path.read_text()) or {}
data["allowed_fapi_latency"] = allowed_fapi_latency
data["allowed_tick_error"] = allowed_tick_error
data["l2a_allowed_latency"] = l2a_allowed_latency
data["timer_thread_wakeup_threshold"] = timer_thread_wakeup_threshold

message_thread = data.setdefault("message_thread_config", {})
message_thread["cpu_affinity"] = message_thread_cpu

transport = data.setdefault("transport", {})
app_config = transport.setdefault("app_config", {})
app_config["pcap_enable"] = 0
app_config["pcap_shm_caching_cpu_core"] = -1
app_config["pcap_file_saving_cpu_core"] = -1

path.write_text(yaml.safe_dump(data, sort_keys=False))
PY
}

resolve_l2_config() {
  local config=$1
  local l2_file
  l2_file=$(awk -F': *' '/^l2adapter_filename:/ {print $2; exit}' "$config" | tr -d "\"'")
  if [ -z "$l2_file" ]; then
    return 0
  fi
  printf '%s/%s\n' "$CONFIG_DIR" "$l2_file"
}

# Mount hugepages for DPDK
mkdir -p /dev/hugepages
mount -t hugetlbfs nodev /dev/hugepages 2>/dev/null || true

DEFAULT_PROFILE=${AERIAL_L1_PROFILE:-P5G_GH}
PROFILE=${1:-$DEFAULT_PROFILE}
if [ "$#" -gt 0 ]; then
  shift
fi

CONFIG_DIR=/opt/nvidia/aerial/cuPHY-CP/cuphycontroller/config
CONFIG=
L2_CONFIG=

# OCUDU integration here uses nvIPC FAPI only; skip FH NIC bring-up unless explicitly overridden.
: "${AERIAL_SKIP_NIC_REG:=1}"
export AERIAL_SKIP_NIC_REG

if [ "$PROFILE" = "loopback" ]; then
  SOURCE_PROFILE=${AERIAL_BASE_PROFILE:-P5G_GH}
  SOURCE_CONFIG="$CONFIG_DIR/cuphycontroller_${SOURCE_PROFILE}.yaml"
  CONFIG="$CONFIG_DIR/cuphycontroller_loopback.yaml"
  [ -f "$SOURCE_CONFIG" ] || fail "missing source profile config: $SOURCE_CONFIG"
  python3 /usr/local/bin/make_loopback_config.py "$SOURCE_CONFIG" "$CONFIG"
  echo "Generated standalone loopback config for GB10 from $SOURCE_PROFILE"
else
  CONFIG="$CONFIG_DIR/cuphycontroller_${PROFILE}.yaml"
fi

[ -f "$CONFIG" ] || fail "missing Aerial profile config: $CONFIG"
L2_CONFIG=$(resolve_l2_config "$CONFIG")

apply_gb10_sm_clamps "$CONFIG"

if [ "$AERIAL_SKIP_NIC_REG" != "0" ]; then
  sed -i "s/gpu_init_comms_dl: 1/gpu_init_comms_dl: 0/" "$CONFIG" 2>/dev/null || true
  sed -i "s/dl_iq_data_fmt: {comp_meth: 1, bit_width: 14}/dl_iq_data_fmt: {comp_meth: 0, bit_width: 16}/" "$CONFIG" 2>/dev/null || true
  if [ -n "$L2_CONFIG" ] && [ -f "$L2_CONFIG" ]; then
    tune_no_nic_l2_config "$L2_CONFIG"
  fi
fi

echo "Starting Aerial profile: $PROFILE"
echo "Using config: $CONFIG"

cd /opt/nvidia/aerial/build
exec ./cuPHY-CP/cuphycontroller/examples/cuphycontroller_scf "$PROFILE" "$@"
