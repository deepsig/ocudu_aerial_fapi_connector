#!/usr/bin/env bash
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

set -euo pipefail

INTEGRATION_DURATION=${1:-30}
BRIDGE_TIMEOUT=${AERIAL_BRIDGE_TEST_TIMEOUT:-20}
SUITE_DIR=${AERIAL_SUITE_LOG_DIR:-/tmp/aerial_validation}
BRIDGE_DIR="$SUITE_DIR/bridge_test"
INTEGRATION_DIR="$SUITE_DIR/integration"
WAVEFORM_PATTERN=${AERIAL_WAVEFORM_PATTERN:-}
WAVEFORM_DIR="$SUITE_DIR/waveform"
TOTAL_STEPS=2
BRIDGE_L1_PID=""

if [[ -n "$WAVEFORM_PATTERN" ]]; then
  TOTAL_STEPS=3
fi

mkdir -p "$BRIDGE_DIR" "$INTEGRATION_DIR"

wait_for_l1_exit() {
  pkill -f cuphycontroller_scf 2>/dev/null || true
  for _ in $(seq 1 10); do
    if ! pgrep -f cuphycontroller_scf >/dev/null 2>&1; then
      break
    fi
    sleep 1
  done
  sleep 2
}

start_bridge_l1() {
  local attempt
  for attempt in 1 2 3; do
    : > "$BRIDGE_DIR/l1.log"
    /usr/local/bin/start_l1.sh > "$BRIDGE_DIR/l1.log" 2>&1 &
    BRIDGE_L1_PID=$!
    sleep 12
    if kill -0 "$BRIDGE_L1_PID" 2>/dev/null; then
      return 0
    fi
    kill "$BRIDGE_L1_PID" 2>/dev/null || true
    sleep 1
    kill -9 "$BRIDGE_L1_PID" 2>/dev/null || true
    BRIDGE_L1_PID=""
    wait_for_l1_exit
  done
  return 1
}

cleanup() {
  if [[ -n "${BRIDGE_L1_PID}" ]]; then
    kill "$BRIDGE_L1_PID" 2>/dev/null || true
    sleep 1
    kill -9 "$BRIDGE_L1_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

echo "============================================"
echo "  OCUDU + Aerial Validation Suite"
echo "  bridge_test timeout: ${BRIDGE_TIMEOUT}s"
echo "  integration duration: ${INTEGRATION_DURATION}s"
if [[ -n "$WAVEFORM_PATTERN" ]]; then
  echo "  waveform pattern: ${WAVEFORM_PATTERN}"
fi
echo "============================================"
echo ""

echo "[1/${TOTAL_STEPS}] bridge_test coverage"
if ! start_bridge_l1; then
  echo "ERROR: bridge_test L1 startup failed"
  tail -40 "$BRIDGE_DIR/l1.log" || true
  exit 1
fi

timeout "$BRIDGE_TIMEOUT" /opt/aerial_bridge/build/bridge_test \
  > "$BRIDGE_DIR/bridge_test.log" 2>&1 || true

kill "$BRIDGE_L1_PID" 2>/dev/null || true
sleep 1
kill -9 "$BRIDGE_L1_PID" 2>/dev/null || true
BRIDGE_L1_PID=""
wait_for_l1_exit

BRIDGE_OK=1
grep -q "CONFIG_RESPONSE: error_code=0x00" "$BRIDGE_DIR/bridge_test.log" || BRIDGE_OK=0
grep -q "START_REQUEST sent" "$BRIDGE_DIR/bridge_test.log" || BRIDGE_OK=0
BRIDGE_SLOT_COUNT=$(grep -c "SLOT_IND #" "$BRIDGE_DIR/bridge_test.log" || true)

echo "  bridge slots observed: $BRIDGE_SLOT_COUNT"
if [[ "$BRIDGE_OK" -ne 1 || "$BRIDGE_SLOT_COUNT" -le 0 ]]; then
  echo "ERROR: bridge_test coverage failed"
  tail -80 "$BRIDGE_DIR/bridge_test.log" || true
  exit 1
fi

echo ""
echo "[2/${TOTAL_STEPS}] full OCUDU + Aerial integration"
INTEGRATION_RUN_OK=0
for attempt in 1 2; do
  if AERIAL_LOG_DIR="$INTEGRATION_DIR" AERIAL_CHARACTERIZE=0 \
      /opt/aerial_bridge/scripts/run_integration_test.sh "$INTEGRATION_DURATION" \
      > "$INTEGRATION_DIR/run.log" 2>&1; then
    INTEGRATION_RUN_OK=1
    break
  fi
  killall -q cuphycontroller_scf gnb 2>/dev/null || true
  wait_for_l1_exit
done

if [[ "$INTEGRATION_RUN_OK" -ne 1 ]]; then
  echo "ERROR: integration run failed to start cleanly"
  tail -80 "$INTEGRATION_DIR/run.log" || true
  exit 1
fi

python3 /opt/aerial_bridge/scripts/characterize_integration.py \
  --log-dir "$INTEGRATION_DIR" \
  --duration "$INTEGRATION_DURATION" \
  | tee "$INTEGRATION_DIR/characterize.log"

INTEGRATION_OK=1
grep -q -- "- overall: PASS" "$INTEGRATION_DIR/characterize.log" || INTEGRATION_OK=0
grep -q -- "- ul_structural_sanity: PASS" "$INTEGRATION_DIR/characterize.log" || INTEGRATION_OK=0

if [[ -n "$WAVEFORM_PATTERN" ]]; then
  echo ""
  echo "[3/${TOTAL_STEPS}] waveform-backed Aerial PHY validation"
  mkdir -p "$WAVEFORM_DIR"
  AERIAL_WAVEFORM_LOG_DIR="$WAVEFORM_DIR" \
    /opt/aerial_bridge/scripts/run_waveform_validation.sh "$WAVEFORM_PATTERN" \
    | tee "$WAVEFORM_DIR/run.log"
fi

echo ""
echo "Artifacts"
echo "  bridge_test:   $BRIDGE_DIR"
echo "  integration:   $INTEGRATION_DIR"
if [[ -n "$WAVEFORM_PATTERN" ]]; then
  echo "  waveform:      $WAVEFORM_DIR"
fi

if [[ "$INTEGRATION_OK" -ne 1 ]]; then
  echo "ERROR: integration coverage failed"
  exit 1
fi

echo "Suite result: PASS"
