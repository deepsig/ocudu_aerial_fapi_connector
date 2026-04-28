<!-- Copyright (c) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Changelog

## 2026-04-28

### Changed

- Upgraded the default Aerial source/build target from the older `25.x` line to
  `26.1.0`, using the `26-1-cubb` NGC base image.
- Standardized the repo on the external OCUDU `L2/L3 -> nvIPC -> Aerial L1`
  path as the validated local integration mode.
- Updated the launcher wrappers and docs to treat `--shm-size=16g` as a
  required runtime precondition for local validation.
- Expanded the README/design docs with the tuned local validation defaults and
  an explicit FH-enabled / real O-RU bring-up runbook, clearly marked as
  unvalidated radio-in-loop guidance on GB10.

### Added

- `scripts/run_validation_suite.sh` for a single-command local validation pass
  covering both `bridge_test` and the full OCUDU integration path.
- `scripts/characterize_integration.py` coverage/signoff reporting for runtime
  health, channel coverage, and UL structural sanity.
- `scripts/check_waveform_assets.py` and
  `scripts/run_waveform_validation.sh` for `nrSim` waveform preflight and
  sibling PHY validation.
- `aerial_bridge/patches/standalone_filename_fix.py` for Aerial `26.1.0`
  standalone loopback startup.
- `aerial_bridge/patches/nvipc_host_pinning_fix.py` to tolerate GB10 nvIPC SHM
  host-page-lock failure in the no-NIC path.

### Fixed

- Hardened the no-NIC Aerial overlay so `AERIAL_SKIP_NIC_REG=1` skips fronthaul
  NIC registration instead of entering `registerNic()` and only downgrading the
  failure afterward.
- Folded GB10-specific launch behavior into `scripts/start_l1.sh`:
  SM clamps, no-NIC transport tuning, disabled upstream `26.1.0` nvIPC PCAP
  defaults, and the timer/message-thread settings that stabilized the no-NIC
  local path on GB10.
- Corrected `scripts/run_integration_test.sh` log-path handling so the suite can
  collect artifacts from a caller-selected log directory.
- Pinned the OCUDU bridge RX thread in the local harness and taught the
  characterizer to distinguish shutdown-tail bookkeeping noise from real
  runtime-health failures.

### Removed

- Dropped `aerial_bridge/patches/gdrcopy_fix.py` and
  `aerial_bridge/patches/gpinned_buffer_fix.py` from the active build because
  their core GPUDirect/GDR fallback behavior is upstream in Aerial `26.1.0`.
- Removed `aerial_bridge/patches/fh_tx_nonfatal_fix.py` from the active branch.

### Validation Snapshot

- Local runtime: Aerial `26.1.0` / `26-1-cubb` with current-tree wrapper scripts
- `bridge_test`: PASS
  - `CONFIG_RESPONSE: error_code=0x00`
  - `START_REQUEST`
  - continuous `SLOT_IND`
- Full OCUDU integration: PASS in the latest local 20s suite rerun
  - `CONFIG/START`: PASS
  - `PDSCH`: PASS
  - `PUSCH`: PASS
  - `PUCCH F0/F1`: PASS
  - `ul_structural_sanity`: PASS
  - runtime health: PASS (`aerial_err_ind=0`, `l1_tick_error_alerts=0`)

### Known Limitations

- The no-NIC `26.1.0` path now passes the current short local suite, but
  longer soak coverage is still open.
- True UL payload correctness is still unvalidated without a waveform source or
  a real RU/UE path.
- The WNC / real O-RU path is documented but not signed off in this repo.
