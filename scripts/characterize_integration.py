#!/usr/bin/env python3
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

import argparse
import re
from collections import Counter
from pathlib import Path


BRIDGE_CRC_RE = re.compile(
    r"CRC\.indication #(?P<seq>\d+) sfn=(?P<sfn>\d+) slot=(?P<slot>\d+) "
    r"rnti=0x(?P<rnti>[0-9a-fA-F]+) harq=(?P<harq>\d+) ok=(?P<ok>\d+)"
)
BRIDGE_UCI_PUSCH_RE = re.compile(
    r"UCI\.indication #(?P<seq>\d+) sfn=(?P<sfn>\d+) slot=(?P<slot>\d+) "
    r"type=PUSCH rnti=0x(?P<rnti>[0-9a-fA-F]+) harq=(?P<harq>\d+) "
    r"csi1=(?P<csi1>\d+) csi2=(?P<csi2>\d+)"
)
BRIDGE_UCI_PUCCH01_RE = re.compile(
    r"UCI\.indication #(?P<seq>\d+) sfn=(?P<sfn>\d+) slot=(?P<slot>\d+) "
    r"type=PUCCH01 fmt=(?P<fmt>\d+) rnti=0x(?P<rnti>[0-9a-fA-F]+) "
    r"sr=(?P<sr>\d+) harq_bits=(?P<harq_bits>\d+)"
)
BRIDGE_UCI_PUCCH234_RE = re.compile(
    r"UCI\.indication #(?P<seq>\d+) sfn=(?P<sfn>\d+) slot=(?P<slot>\d+) "
    r"type=PUCCH234 fmt=(?P<fmt>\d+) rnti=0x(?P<rnti>[0-9a-fA-F]+) "
    r"sr_bits=(?P<sr_bits>\d+) harq=(?P<harq>\d+) csi1=(?P<csi1>\d+) csi2=(?P<csi2>\d+)"
)
UL_PUSCH_SERIALIZE_RE = re.compile(
    r"UL serialize type=PUSCH sfn=(?P<sfn>\d+) slot=(?P<slot>\d+) "
    r"rnti=0x(?P<rnti>[0-9a-fA-F]+) fmt/layers=(?P<layers>\d+) "
    r"prb=\[(?P<prb_start>\d+),(?P<prb_size>\d+)\] "
    r"sym=\[(?P<sym_start>\d+),(?P<sym_len>\d+)\] "
    r"harq_id/count=(?P<harq>\d+) harq_bits=(?P<harq_bits>\d+) "
    r"sr=(?P<sr>\d+) csi1=(?P<csi1>\d+)"
)
UL_PUCCH_SERIALIZE_RE = re.compile(
    r"UL serialize type=PUCCH sfn=(?P<sfn>\d+) slot=(?P<slot>\d+) "
    r"rnti=0x(?P<rnti>[0-9a-fA-F]+) fmt/layers=(?P<fmt>\d+) "
    r"prb=\[(?P<prb_start>\d+),(?P<prb_size>\d+)\] "
    r"sym=\[(?P<sym_start>\d+),(?P<sym_len>\d+)\] "
    r"harq_id/count=(?P<harq_count>\d+) harq_bits=(?P<harq_bits>\d+) "
    r"sr=(?P<sr>\d+) csi1=(?P<csi1>\d+)"
)
L1_PUSCH_PARSE_RE = re.compile(
    r"FAPI PUSCH parse SFN (?P<sfn>\d+)\.(?P<slot>\d+) rnti=(?P<rnti>\d+) "
    r"harqProcessId=(?P<harq>\d+) rv=(?P<rv>\d+) ndi=(?P<ndi>\d+) "
    r"tbSize=(?P<tb_size>\d+) startPrb=(?P<prb_start>\d+) nPrb=(?P<prb_size>\d+) "
    r"startSym=(?P<sym_start>\d+) nSym=(?P<sym_len>\d+)"
)
L1_CRC_EMIT_RE = re.compile(
    r"FAPI CRC emit SFN (?P<sfn>\d+)\.(?P<slot>\d+) rnti=(?P<rnti>\d+) "
    r"harq_id=(?P<harq>\d+) tb_crc_status=(?P<status>\d+) handle=(?P<handle>\d+)"
)
PDSCH_RE = re.compile(
    r"\[pdsch_cw0\].*mcsTable=(?P<mcs_table>\d+) mcs=(?P<mcs>\d+) "
    r"tcr=(?P<tcr>\d+) qm=(?P<qm>\d+).*tbSize=(?P<tb_size>\d+)"
)
SUMMARY_RE = re.compile(r"\[aerial_bridge\]\[summary\] (?P<body>.+)")


def count_substring(text: str, pattern: str) -> int:
    return text.count(pattern)


def load_text(path: Path) -> str:
    return path.read_text(errors="ignore") if path.exists() else ""


def status_mark(ok: bool) -> str:
    return "PASS" if ok else "FAIL"


def observed_mark(observed: bool, configured: bool = False) -> str:
    if observed:
        return "PASS"
    if configured:
        return "CONFIGURED"
    return "NOT OBSERVED"


def count_shutdown_tail_ul_sfn_err_ind(l1_log: str) -> int:
    lines = l1_log.splitlines()
    shutdown_markers = (
        "PhyDriverCtx destructor starting",
        "[signal_handler] received signal 15",
        "Thread msg_processing exiting",
    )
    count = 0
    for i, line in enumerate(lines):
        if "Send Err.ind" not in line or "msg_id=0x81" not in line or "err_code=0x03" not in line:
            continue
        if i == 0 or "check_sfn_slot: SFN mismatch" not in lines[i - 1]:
            continue
        if "msg_id=0x81 dropped" not in lines[i - 1]:
            continue
        tail = lines[i + 1 : i + 13]
        if any(marker in tail_line for tail_line in tail for marker in shutdown_markers):
            count += 1
    return count


def parse_summary(gnb_stdout: str) -> dict[str, int]:
    summary_match = SUMMARY_RE.search(gnb_stdout)
    summary: dict[str, int] = {}
    if not summary_match:
        return summary
    for token in summary_match.group("body").split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        try:
            summary[key] = int(value)
        except ValueError:
            pass
    return summary


def bridge_ul_key(match: re.Match[str]) -> tuple[int, int, int, int]:
    return (
        int(match.group("sfn")),
        int(match.group("slot")),
        int(match.group("rnti"), 16),
        int(match.group("harq")),
    )


def bridge_ul_shape(match: re.Match[str]) -> tuple[int, int, int, int]:
    return (
        int(match.group("prb_start")),
        int(match.group("prb_size")),
        int(match.group("sym_start")),
        int(match.group("sym_len")),
    )


def l1_ul_key(match: re.Match[str]) -> tuple[int, int, int, int]:
    return (
        int(match.group("sfn")),
        int(match.group("slot")),
        int(match.group("rnti")),
        int(match.group("harq")),
    )


def l1_ul_shape(match: re.Match[str]) -> tuple[int, int, int, int]:
    return (
        int(match.group("prb_start")),
        int(match.group("prb_size")),
        int(match.group("sym_start")),
        int(match.group("sym_len")),
    )


def bridge_crc_key(match: re.Match[str]) -> tuple[int, int, int, int]:
    return (
        int(match.group("sfn")),
        int(match.group("slot")),
        int(match.group("rnti"), 16),
        int(match.group("harq")),
    )


def sample_pucch_schedule_key(match: re.Match[str]) -> tuple[int, int, int]:
    return (
        int(match.group("sfn")),
        int(match.group("slot")),
        int(match.group("rnti"), 16),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Characterize OCUDU+Aerial no-NIC integration logs")
    parser.add_argument("--log-dir", default="/tmp/aerial_integration", help="Directory containing l1.log/gnb.log/gnb_stdout.log")
    parser.add_argument("--duration", type=float, required=True, help="Nominal test duration in seconds")
    args = parser.parse_args()

    log_dir = Path(args.log_dir)
    l1_log = load_text(log_dir / "l1.log")
    gnb_log = load_text(log_dir / "gnb.log")
    gnb_stdout = load_text(log_dir / "gnb_stdout.log")

    summary = parse_summary(gnb_stdout)

    bridge_crc_matches = list(BRIDGE_CRC_RE.finditer(gnb_stdout))
    bridge_uci_pusch_matches = list(BRIDGE_UCI_PUSCH_RE.finditer(gnb_stdout))
    bridge_uci_pucch01_matches = list(BRIDGE_UCI_PUCCH01_RE.finditer(gnb_stdout))
    bridge_uci_pucch234_matches = list(BRIDGE_UCI_PUCCH234_RE.finditer(gnb_stdout))
    bridge_ul_pusch_matches = list(UL_PUSCH_SERIALIZE_RE.finditer(gnb_stdout))
    bridge_ul_pucch_matches = list(UL_PUCCH_SERIALIZE_RE.finditer(gnb_stdout))
    l1_pusch_parse_matches = list(L1_PUSCH_PARSE_RE.finditer(l1_log))
    l1_crc_emit_matches = list(L1_CRC_EMIT_RE.finditer(l1_log))
    pdsch_matches = list(PDSCH_RE.finditer(l1_log))

    crc_total = summary.get("crc_total", len(bridge_crc_matches))
    crc_ok = summary.get("crc_ok", sum(1 for m in bridge_crc_matches if m.group("ok") == "1"))
    crc_fail = crc_total - crc_ok
    ul_bler = (crc_fail / crc_total * 100.0) if crc_total else 0.0

    pdsch_tb_bytes = sum(int(m.group("tb_size")) for m in pdsch_matches)
    tx_data_reqs = summary.get("tx_data_reqs", count_substring(gnb_stdout, "TX_DATA.request"))
    tx_data_bytes = summary.get("tx_data_bytes", pdsch_tb_bytes)
    pdsch_observed = bool(pdsch_matches) or tx_data_reqs > 0 or tx_data_bytes > 0
    dl_sched_mbps = (tx_data_bytes * 8.0 / args.duration / 1e6) if args.duration > 0 else 0.0

    counters = Counter()
    counters["slot_jumps"] = count_substring(gnb_log, "Unexpected jump in slot indications")
    counters["dl_queue_full"] = count_substring(gnb_log, "DL task queue is full")
    counters["forced_nack"] = count_substring(gnb_log, 'Forcing "NACK"')
    counters["uci_discard"] = count_substring(gnb_log, "Discarding UCI indication PDU")
    counters["err_ind"] = count_substring(l1_log, "Send Err.ind")
    counters["shutdown_tail_err_ind"] = count_shutdown_tail_ul_sfn_err_ind(l1_log)
    counters["runtime_err_ind"] = counters["err_ind"] - counters["shutdown_tail_err_ind"]
    counters["agg_exhaustion"] = count_substring(l1_log, "No available Aggr")
    counters["crc_mismatch"] = count_substring(gnb_stdout, "Failed to set CRC value")
    counters["uci_mismatch"] = count_substring(gnb_stdout, "Failed to set UCI value")
    counters["tick_error_alerts"] = count_substring(l1_log, "[TICK.ERROR]")
    counters["rx_data"] = summary.get("rx_data_pdus", count_substring(gnb_stdout, "RX_DATA.indication"))
    counters["rach_ind"] = count_substring(gnb_stdout, "RACH.indication")
    counters["srs_ind"] = summary.get("srs_total", count_substring(gnb_stdout, "SRS.indication"))

    bridge_ul_pusch_pdus = summary.get("ul_pusch_pdus", len(bridge_ul_pusch_matches))
    bridge_ul_pucch_pdus = summary.get("ul_pucch_pdus", len(bridge_ul_pucch_matches))
    bridge_ul_prach_pdus = summary.get("ul_prach_pdus", 0)
    bridge_ul_srs_pdus = summary.get("ul_srs_pdus", 0)

    uci_pusch_total = summary.get("uci_pusch", len(bridge_uci_pusch_matches))
    uci_pucch01_total = summary.get("uci_pucch01", len(bridge_uci_pucch01_matches))
    uci_pucch234_total = summary.get("uci_pucch234", len(bridge_uci_pucch234_matches))

    sample_bridge_pusch = {bridge_ul_key(m): bridge_ul_shape(m) for m in bridge_ul_pusch_matches}
    sample_l1_pusch = {l1_ul_key(m): l1_ul_shape(m) for m in l1_pusch_parse_matches}
    sample_bridge_crc = {bridge_crc_key(m) for m in bridge_crc_matches if int(m.group("seq")) <= 16}
    sample_l1_crc = {l1_ul_key(m) for m in l1_crc_emit_matches[:16]}
    sample_bridge_pucch = {
        sample_pucch_schedule_key(m): (
            int(m.group("fmt")),
            int(m.group("harq_bits")),
            int(m.group("sr")),
        )
        for m in bridge_ul_pucch_matches
    }
    sample_bridge_pucch01 = {
        sample_pucch_schedule_key(m): (
            int(m.group("fmt")),
            int(m.group("harq_bits")),
            int(m.group("sr")),
        )
        for m in bridge_uci_pucch01_matches
        if int(m.group("seq")) <= 16
    }

    sample_pusch_missing_in_l1 = len(set(sample_bridge_pusch) - set(sample_l1_pusch))
    sample_pusch_shape_mismatch = sum(
        1
        for key, shape in sample_bridge_pusch.items()
        if key in sample_l1_pusch and sample_l1_pusch[key] != shape
    )
    sample_crc_missing_in_bridge = len(sample_l1_crc - sample_bridge_crc)
    sample_crc_missing_in_l1 = len(sample_bridge_crc - sample_l1_crc)
    sample_pucch_missing_in_uci = len(set(sample_bridge_pucch) - set(sample_bridge_pucch01))
    sample_pucch_field_mismatch = sum(
        1
        for key, fields in sample_bridge_pucch.items()
        if key in sample_bridge_pucch01 and sample_bridge_pucch01[key] != fields
    )

    pusch_crc_delta = bridge_ul_pusch_pdus - crc_total
    pusch_uci_delta = bridge_ul_pusch_pdus - uci_pusch_total
    ul_shutdown_tail_budget = 2 if (
        counters["runtime_err_ind"] == 0
        and counters["forced_nack"] == 0
        and counters["uci_discard"] == 0
        and counters["tick_error_alerts"] == 0
    ) else 1
    pusch_count_exact = (
        0 <= pusch_crc_delta <= ul_shutdown_tail_budget
        and 0 <= pusch_uci_delta <= ul_shutdown_tail_budget
    )
    pucch_count_exact = bridge_ul_pucch_pdus == (uci_pucch01_total + uci_pucch234_total)
    ul_structural_sanity = (
        bridge_ul_pusch_pdus > 0
        and pusch_count_exact
        and pucch_count_exact
        and sample_pusch_missing_in_l1 == 0
        and sample_pusch_shape_mismatch == 0
        and sample_crc_missing_in_bridge == 0
        and sample_crc_missing_in_l1 == 0
        and sample_pucch_missing_in_uci == 0
        and sample_pucch_field_mismatch == 0
        and counters["crc_mismatch"] == 0
        and counters["uci_mismatch"] == 0
    )

    unique_ul_rntis = sorted({bridge_ul_key(m)[2] for m in bridge_ul_pusch_matches} | {bridge_crc_key(m)[2] for m in bridge_crc_matches})
    unique_crc_harqs = sorted({int(m.group("harq")) for m in bridge_crc_matches[:32]})

    ssb_configured = "cuPHY SSB channel object" in l1_log
    pdcch_configured = "cuPHY PDCCH channel object" in l1_log
    prach_configured = "cuPHY PRACH channel object" in l1_log
    srs_configured = "cuPHY SRS channel object" in l1_log

    all_clean = all(
        counters[k] == 0
        for k in {
            "slot_jumps",
            "dl_queue_full",
            "forced_nack",
            "uci_discard",
            "runtime_err_ind",
            "agg_exhaustion",
            "crc_mismatch",
            "uci_mismatch",
        }
    )

    print("Integration Characterization")
    print(f"log_dir: {log_dir}")
    print(f"duration_s: {args.duration:g}")
    print()

    print("Runtime Health")
    print(f"- overall: {status_mark(all_clean)}")
    print(f"- slot_jumps: {counters['slot_jumps']}")
    print(f"- dl_queue_full: {counters['dl_queue_full']}")
    print(f"- forced_nack: {counters['forced_nack']}")
    print(f"- uci_discard: {counters['uci_discard']}")
    print(f"- aerial_err_ind: {counters['runtime_err_ind']}")
    print(f"- aerial_err_ind_total: {counters['err_ind']}")
    print(f"- shutdown_tail_err_ind: {counters['shutdown_tail_err_ind']}")
    print(f"- aggregate_exhaustion: {counters['agg_exhaustion']}")
    print(f"- bridge_crc_mismatch_logs: {counters['crc_mismatch']}")
    print(f"- bridge_uci_mismatch_logs: {counters['uci_mismatch']}")
    print(f"- l1_tick_error_alerts: {counters['tick_error_alerts']}")
    print()

    print("Radio Quality")
    print(f"- ul_crc_total: {crc_total}")
    print(f"- ul_crc_ok: {crc_ok}")
    print(f"- ul_crc_fail: {crc_fail}")
    print(f"- raw_ul_bler_pct: {ul_bler:.2f}")
    print(f"- dl_pdsch_tb_count: {len(pdsch_matches)}")
    print(f"- dl_pdsch_tb_total_bytes: {pdsch_tb_bytes}")
    print(f"- dl_scheduled_throughput_mbps: {dl_sched_mbps:.3f}")
    print(f"- ul_rx_data_indications: {counters['rx_data']}")
    print(f"- ul_rx_data_bytes: {summary.get('rx_data_bytes', 0)}")
    print(f"- dl_tx_data_bytes: {tx_data_bytes}")
    if counters["rx_data"] == 0:
        print("- ul_userplane_throughput: unavailable from current logs (no RX_DATA indications logged)")
    else:
        ul_rx_mbps = summary.get("rx_data_bytes", 0) * 8.0 / args.duration / 1e6
        print(f"- ul_userplane_throughput_mbps: {ul_rx_mbps:.3f}")
    print()

    print("UL Structural Sanity")
    print(f"- ul_structural_sanity: {status_mark(ul_structural_sanity)}")
    print(f"- ul_pusch_pdus_scheduled: {bridge_ul_pusch_pdus}")
    print(f"- ul_pusch_crc_indications: {crc_total}")
    print(f"- ul_pusch_uci_indications: {uci_pusch_total}")
    print(f"- ul_pusch_schedule_to_crc_delta: {pusch_crc_delta}")
    print(f"- ul_pusch_schedule_to_uci_delta: {pusch_uci_delta}")
    print(f"- ul_shutdown_tail_budget: {ul_shutdown_tail_budget}")
    print(f"- ul_pucch_pdus_scheduled: {bridge_ul_pucch_pdus}")
    print(f"- ul_pucch_uci_indications: {uci_pucch01_total + uci_pucch234_total}")
    print(f"- ul_prach_pdus_scheduled: {bridge_ul_prach_pdus}")
    print(f"- ul_srs_pdus_scheduled: {bridge_ul_srs_pdus}")
    print(f"- sample_pusch_schedule_entries_logged: {len(sample_bridge_pusch)}")
    print(f"- sample_pusch_missing_in_l1_parse: {sample_pusch_missing_in_l1}")
    print(f"- sample_pusch_shape_mismatches: {sample_pusch_shape_mismatch}")
    print(f"- sample_crc_missing_in_bridge_logs: {sample_crc_missing_in_bridge}")
    print(f"- sample_crc_missing_in_l1_logs: {sample_crc_missing_in_l1}")
    print(f"- sample_pucch_missing_in_uci: {sample_pucch_missing_in_uci}")
    print(f"- sample_pucch_field_mismatches: {sample_pucch_field_mismatch}")
    if unique_ul_rntis:
        rnti_list = ", ".join(f"0x{rnti:04x}" for rnti in unique_ul_rntis)
        print(f"- ul_rntis_observed: {rnti_list}")
    else:
        print("- ul_rntis_observed: none")
    if unique_crc_harqs:
        harq_list = ", ".join(str(harq) for harq in unique_crc_harqs)
        print(f"- sample_harq_ids_observed: {harq_list}")
    else:
        print("- sample_harq_ids_observed: none")
    print()

    print("Interpretation")
    print("- raw_ul_bler_pct is Aerial's native CRC result. In the current no-UE/no-RF path, all-fail CRC is expected.")
    print("- ul_structural_sanity answers a different question: whether OCUDU scheduled UL grants and received coherent CRC/UCI back for those grants.")
    print("- true UL payload correctness is still UNVALIDATED in this mode because there is no real UE/RU waveform source and RX_DATA bytes remain zero.")
    print(f"- effective_test_mode_quality: {status_mark(all_clean)}")
    print("- effective_test_mode_quality reflects scheduler/runtime health, not OTA decode quality.")
    print()

    print("UCI Coverage")
    print(f"- pusch_uci_count: {uci_pusch_total}")
    print(f"- pucch01_uci_count: {uci_pucch01_total}")
    print(f"- pucch234_uci_count: {uci_pucch234_total}")
    print()

    print("Channel Signoff")
    config_start_seen = "CONFIG_RESPONSE" in gnb_stdout and (
        "START_REQUEST sent" in gnb_stdout or "Sent START_REQUEST" in gnb_stdout
    )
    print(f"- CONFIG/START: {observed_mark(config_start_seen)}")
    print(f"- SSB/PBCH: {observed_mark(False, ssb_configured)}")
    print(f"- PDCCH: {observed_mark(False, pdcch_configured)}")
    print(f"- PDSCH: {observed_mark(pdsch_observed)}")
    print(f"- PUSCH: {observed_mark(bridge_ul_pusch_pdus > 0)}")
    print(f"- PUCCH F0/F1: {observed_mark(uci_pucch01_total > 0 and sample_pucch_missing_in_uci == 0)}")
    print(f"- PUCCH F2/F3/F4: {observed_mark(uci_pucch234_total > 0)}")
    print(f"- PRACH: {observed_mark(counters['rach_ind'] > 0, prach_configured)}")
    print(f"- SRS: {observed_mark(counters['srs_ind'] > 0, srs_configured)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
