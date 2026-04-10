#!/usr/bin/env bash
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# Start cuphycontroller_scf L1 on DGX Spark GB10.
# Requires: --gpus all --privileged --shm-size=16g --network host.
# Default mode is the validated no-NIC nvIPC/FAPI path.
# Set AERIAL_SKIP_NIC_REG=0 to attempt real FH/NIC bring-up instead.
set -e

# Mount hugepages for DPDK
mkdir -p /dev/hugepages
mount -t hugetlbfs nodev /dev/hugepages 2>/dev/null || true

PROFILE=${1:-P5G_GH}
if [ "$#" -gt 0 ]; then
  shift
fi

CONFIG_DIR=/opt/nvidia/aerial/cuPHY-CP/cuphycontroller/config

# OCUDU integration here uses nvIPC FAPI only; skip FH NIC bring-up unless explicitly overridden.
: "${AERIAL_SKIP_NIC_REG:=1}"
export AERIAL_SKIP_NIC_REG

if [ "$AERIAL_SKIP_NIC_REG" != "0" ]; then
  CONFIG="$CONFIG_DIR/cuphycontroller_P5G_GH.yaml"
  L2_CONFIG="$CONFIG_DIR/l2_adapter_config_P5G_GH.yaml"
  sed -i "s/gpu_init_comms_dl: 1/gpu_init_comms_dl: 0/" "$CONFIG" 2>/dev/null || true
  sed -i "s/dl_iq_data_fmt: {comp_meth: 1, bit_width: 14}/dl_iq_data_fmt: {comp_meth: 0, bit_width: 16}/" "$CONFIG" 2>/dev/null || true
  sed -i "s/allowed_fapi_latency: 0/allowed_fapi_latency: 16/" "$L2_CONFIG" 2>/dev/null || true
  if grep -q '^l2a_allowed_latency:' "$L2_CONFIG" 2>/dev/null; then
    sed -i "s/^l2a_allowed_latency:.*/l2a_allowed_latency: 1000000/" "$L2_CONFIG" 2>/dev/null || true
  else
    python3 - "$L2_CONFIG" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
marker = "allowed_tick_error: 10\n"
if marker in text:
    text = text.replace(marker, marker + "l2a_allowed_latency: 1000000\n", 1)
else:
    text += "\nl2a_allowed_latency: 1000000\n"
path.write_text(text)
PY
  fi
fi

if [ "$PROFILE" = "loopback" ]; then
  SOURCE_CONFIG=$CONFIG_DIR/cuphycontroller_P5G_GH.yaml
  CONFIG=$CONFIG_DIR/cuphycontroller_loopback.yaml
  python3 /usr/local/bin/make_loopback_config.py "$SOURCE_CONFIG" "$CONFIG"
  echo "Generated standalone loopback config for GB10"
else
  echo "Starting Aerial profile: $PROFILE"
fi

cd /opt/nvidia/aerial/build
exec ./cuPHY-CP/cuphycontroller/examples/cuphycontroller_scf "$PROFILE" "$@"
