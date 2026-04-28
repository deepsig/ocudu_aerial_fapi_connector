#!/usr/bin/env bash
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

set -euo pipefail

PATTERN=${1:-${AERIAL_WAVEFORM_PATTERN:-}}
AERIAL_ROOT=${AERIAL_ROOT:-/opt/nvidia/aerial}
LOG_ROOT=${AERIAL_WAVEFORM_LOG_DIR:-/tmp/aerial_waveform_validation}
MAX_CELLS=${AERIAL_WAVEFORM_MAX_CELLS:-1}
WORK_ROOT="$LOG_ROOT/sdk-root"
TV_SRC=${AERIAL_TV_SRC:-}
ASSET_CHECKER=${AERIAL_WAVEFORM_ASSET_CHECKER:-/opt/aerial_bridge/scripts/check_waveform_assets.py}
PREFLIGHT_ONLY=${AERIAL_WAVEFORM_PREFLIGHT_ONLY:-0}

usage() {
  cat <<'EOF'
usage: run_waveform_validation.sh <nrSim-pattern>

Environment:
  AERIAL_TV_SRC                Optional explicit test-vector source directory.
  AERIAL_WAVEFORM_LOG_DIR      Optional log/output directory.
  AERIAL_WAVEFORM_MAX_CELLS    Optional max cell count passed to copy_test_files.sh.
  AERIAL_WAVEFORM_PREFLIGHT_ONLY
                               If set to 1, verify the asset bundle and exit.

This runs NVIDIA's nrSim waveform-backed DLC test bench against the current
Aerial build. It validates real PHY waveform decode on the same build used by
the OCUDU integration, but it does not inline OCUDU into the waveform path.
EOF
}

require_file() {
  local path=$1
  if [[ ! -e "$path" ]]; then
    echo "ERROR: required path missing: $path" >&2
    exit 1
  fi
}

resolve_tv_source() {
  if [[ -n "$TV_SRC" ]]; then
    if [[ ! -d "$TV_SRC" ]]; then
      echo "ERROR: AERIAL_TV_SRC does not exist: $TV_SRC" >&2
      exit 1
    fi
    return 0
  fi

  local uuid=""
  if [[ -x "$AERIAL_ROOT/5GModel/get_uuid.sh" ]]; then
    uuid=$("$AERIAL_ROOT/5GModel/get_uuid.sh" 2>/dev/null || true)
  fi

  local candidates=()
  if [[ -n "$uuid" ]]; then
    candidates+=("/mnt/cicd_tvs/$uuid/GPU_test_input")
    candidates+=("/mnt/cicd_tvs/$uuid/compact/GPU_test_input")
  fi
  candidates+=("/mnt/cicd_tvs/develop/GPU_test_input")

  local candidate
  for candidate in "${candidates[@]}"; do
    if [[ -d "$candidate" ]]; then
      TV_SRC=$candidate
      return 0
    fi
  done

  cat >&2 <<'EOF'
ERROR: no waveform test-vector source found.
Set AERIAL_TV_SRC to a directory containing NVIDIA GPU_test_input assets, or
mount the usual /mnt/cicd_tvs/... hierarchy into the container.
EOF
  exit 1
}

prepare_tv_source() {
  if [[ "$PATTERN" == "90604" ]] && \
     [[ ! -e "$TV_SRC/launch_pattern_nrSim_90604.yaml" ]] && \
     [[ -e "$TV_SRC/launch_pattern_F08_8C_57.yaml" ]]; then
    local shim_dir="$LOG_ROOT/tv-src-shim"
    rm -rf "$shim_dir"
    mkdir -p "$shim_dir"
    cp -as "$TV_SRC"/. "$shim_dir"/
    ln -sf launch_pattern_F08_8C_57.yaml "$shim_dir/launch_pattern_nrSim_90604.yaml"
    TV_SRC="$shim_dir"
  fi
}

prepare_workspace() {
  rm -rf "$WORK_ROOT"
  mkdir -p \
    "$WORK_ROOT/cuPHY/nvlog" \
    "$WORK_ROOT/cuPHY-CP/cuphycontroller" \
    "$WORK_ROOT/cuPHY-CP/ru-emulator" \
    "$WORK_ROOT/cuPHY-CP/testMAC" \
    "$WORK_ROOT/testVectors/multi-cell" \
    "$LOG_ROOT"

  ln -s "$AERIAL_ROOT/build" "$WORK_ROOT/build"
  ln -s "$AERIAL_ROOT/cubb_scripts" "$WORK_ROOT/cubb_scripts"
  ln -s "$AERIAL_ROOT/5GModel" "$WORK_ROOT/5GModel"

  cp -a "$AERIAL_ROOT/cuPHY/nvlog/config" "$WORK_ROOT/cuPHY/nvlog/"
  cp -a "$AERIAL_ROOT/cuPHY-CP/cuphycontroller/config" "$WORK_ROOT/cuPHY-CP/cuphycontroller/"
  cp -a "$AERIAL_ROOT/cuPHY-CP/ru-emulator/config" "$WORK_ROOT/cuPHY-CP/ru-emulator/"
  cp -a "$AERIAL_ROOT/cuPHY-CP/testMAC/testMAC" "$WORK_ROOT/cuPHY-CP/testMAC/"
  cp -a "$AERIAL_ROOT/testBenches" "$WORK_ROOT/"
}

copy_vectors() {
  "$WORK_ROOT/testBenches/phase4_test_scripts/copy_test_files.sh" \
    "$PATTERN" \
    --src "$TV_SRC" \
    --dst "$WORK_ROOT/testVectors" \
    --max_cells "$MAX_CELLS" \
    > "$LOG_ROOT/copy_test_files.log" 2>&1
}

generate_configs() {
  (
    cd "$WORK_ROOT"
    cuBB_SDK="$WORK_ROOT" python3 "$WORK_ROOT/cubb_scripts/autoconfig/auto_AllConfig.py" \
      -c "$PATTERN" \
      -b "$WORK_ROOT" \
      -p CG1
  ) > "$LOG_ROOT/auto_allconfig.log" 2>&1
}

run_dlc_test_bench() {
  (
    cd "$WORK_ROOT"
    ./build/cuPHY-CP/tests/dlc_test_bench/dlc_test_bench -p "$PATTERN"
  ) > "$LOG_ROOT/dlc_test_bench.log" 2>&1
}

show_tail() {
  local label=$1
  local path=$2
  echo ""
  echo "---- $label ($path) ----"
  tail -80 "$path" || true
}

if [[ -z "$PATTERN" || "$PATTERN" == "-h" || "$PATTERN" == "--help" ]]; then
  usage
  if [[ -z "$PATTERN" ]]; then
    exit 2
  fi
  exit 0
fi

require_file "$AERIAL_ROOT/build/cuPHY-CP/tests/dlc_test_bench/dlc_test_bench"
require_file "$AERIAL_ROOT/cubb_scripts/autoconfig/auto_AllConfig.py"
require_file "$AERIAL_ROOT/testBenches/phase4_test_scripts/copy_test_files.sh"
require_file "$ASSET_CHECKER"
resolve_tv_source
prepare_tv_source
python3 "$ASSET_CHECKER" "$PATTERN" "$TV_SRC"
if [[ "$PREFLIGHT_ONLY" == "1" ]]; then
  echo ""
  echo "Waveform validation preflight result: PASS"
  exit 0
fi
prepare_workspace

echo "============================================"
echo "  Aerial nrSim Waveform Validation"
echo "  pattern:        $PATTERN"
echo "  test vectors:   $TV_SRC"
echo "  work root:      $WORK_ROOT"
echo "  logs:           $LOG_ROOT"
echo "============================================"
echo ""
echo "This validates waveform-backed Aerial PHY decode on the current build."
echo "It is a sibling PHY-quality check, not inline OCUDU waveform injection."
echo ""

if ! copy_vectors; then
  show_tail "copy_test_files" "$LOG_ROOT/copy_test_files.log"
  exit 1
fi

if ! generate_configs; then
  show_tail "auto_AllConfig" "$LOG_ROOT/auto_allconfig.log"
  exit 1
fi

if ! run_dlc_test_bench; then
  show_tail "dlc_test_bench" "$LOG_ROOT/dlc_test_bench.log"
  exit 1
fi

echo "Artifacts"
echo "  copy_test_files: $LOG_ROOT/copy_test_files.log"
echo "  auto_AllConfig:  $LOG_ROOT/auto_allconfig.log"
echo "  dlc_test_bench:  $LOG_ROOT/dlc_test_bench.log"
echo "  staged sdk root: $WORK_ROOT"
echo ""
echo "Waveform validation result: PASS"
