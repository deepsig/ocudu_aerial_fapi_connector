#!/usr/bin/env python3
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

import argparse
import re
from collections import Counter
from pathlib import Path


CRC_RE = re.compile(
    r"CRC\.indication #\d+ sfn=(?P<sfn>\d+) slot=(?P<slot>\d+) rnti=0x(?P<rnti>[0-9a-fA-F]+) harq=(?P<harq>\d+) ok=(?P<ok>\d+)"
)
UCI_PUSCH_RE = re.compile(
    r"UCI\.indication #\d+ sfn=(?P<sfn>\d+) slot=(?P<slot>\d+) type=PUSCH rnti=0x(?P<rnti>[0-9a-fA-F]+) harq=(?P<harq>\d+) csi1=(?P<csi1>\d+) csi2=(?P<csi2>\d+)"
)
UCI_PUCCH01_RE = re.compile(
    r"UCI\.indication #\d+ sfn=(?P<sfn>\d+) slot=(?P<slot>\d+) type=PUCCH01 fmt=(?P<fmt>\d+) rnti=0x(?P<rnti>[0-9a-fA-F]+) sr=(?P<sr>\d+) harq_bits=(?P<harq_bits>\d+)"
)
UCI_PUCCH234_RE = re.compile(
    r"UCI\.indication #\d+ sfn=(?P<sfn>\d+) slot=(?P<slot>\d+) type=PUCCH234 fmt=(?P<fmt>\d+) rnti=0x(?P<rnti>[0-9a-fA-F]+) sr_bits=(?P<sr_bits>\d+) harq=(?P<harq>\d+) csi1=(?P<csi1>\d+) csi2=(?P<csi2>\d+)"
)
PDSCH_RE = re.compile(
    r"\[pdsch_cw0\].*mcsTable=(?P<mcs_table>\d+) mcs=(?P<mcs>\d+) tcr=(?P<tcr>\d+) qm=(?P<qm>\d+).*tbSize=(?P<tb_size>\d+)"
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


def main() -> int:
    parser = argparse.ArgumentParser(description="Characterize OCUDU+Aerial no-NIC integration logs")
    parser.add_argument("--log-dir", default="/tmp/aerial_integration", help="Directory containing l1.log/gnb.log/gnb_stdout.log")
    parser.add_argument("--duration", type=float, required=True, help="Nominal test duration in seconds")
    args = parser.parse_args()

    log_dir = Path(args.log_dir)
    l1_log = load_text(log_dir / "l1.log")
    gnb_log = load_text(log_dir / "gnb.log")
    gnb_stdout = load_text(log_dir / "gnb_stdout.log")

    crc_matches = list(CRC_RE.finditer(gnb_stdout))
    uci_pusch_matches = list(UCI_PUSCH_RE.finditer(gnb_stdout))
    uci_pucch01_matches = list(UCI_PUCCH01_RE.finditer(gnb_stdout))
    uci_pucch234_matches = list(UCI_PUCCH234_RE.finditer(gnb_stdout))
    pdsch_matches = list(PDSCH_RE.finditer(l1_log))
    summary_match = SUMMARY_RE.search(gnb_stdout)
    summary = {}
    if summary_match:
      for token in summary_match.group("body").split():
        if "=" not in token:
          continue
        key, value = token.split("=", 1)
        try:
          summary[key] = int(value)
        except ValueError:
          pass

    crc_total = summary.get("crc_total", len(crc_matches))
    crc_ok = summary.get("crc_ok", sum(1 for m in crc_matches if m.group("ok") == "1"))
    crc_fail = crc_total - crc_ok
    ul_bler = (crc_fail / crc_total * 100.0) if crc_total else 0.0

    pdsch_tb_bytes = sum(int(m.group("tb_size")) for m in pdsch_matches)
    dl_sched_mbps = (summary.get("tx_data_bytes", pdsch_tb_bytes) * 8.0 / args.duration / 1e6) if args.duration > 0 else 0.0

    counters = Counter()
    counters["slot_jumps"] = count_substring(gnb_log, "Unexpected jump in slot indications")
    counters["dl_queue_full"] = count_substring(gnb_log, "DL task queue is full")
    counters["forced_nack"] = count_substring(gnb_log, 'Forcing "NACK"')
    counters["uci_discard"] = count_substring(gnb_log, "Discarding UCI indication PDU")
    counters["err_ind"] = count_substring(l1_log, "Send Err.ind")
    counters["agg_exhaustion"] = count_substring(l1_log, "No available Aggr")
    counters["crc_mismatch"] = count_substring(gnb_stdout, "Failed to set CRC value")
    counters["uci_mismatch"] = count_substring(gnb_stdout, "Failed to set UCI value")
    counters["rx_data"] = summary.get("rx_data_pdus", count_substring(gnb_stdout, "RX_DATA.indication"))
    counters["rach_ind"] = count_substring(gnb_stdout, "RACH.indication")
    counters["srs_ind"] = summary.get("srs_total", count_substring(gnb_stdout, "SRS.indication"))

    ssb_configured = "cuPHY SSB channel object" in l1_log
    pdcch_configured = "cuPHY PDCCH channel object" in l1_log
    prach_configured = "cuPHY PRACH channel object" in l1_log
    srs_configured = "cuPHY SRS channel object" in l1_log

    all_clean = all(v == 0 for k, v in counters.items() if k in {
        "slot_jumps", "dl_queue_full", "forced_nack", "uci_discard", "err_ind", "agg_exhaustion", "crc_mismatch", "uci_mismatch"
    })

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
    print(f"- aerial_err_ind: {counters['err_ind']}")
    print(f"- aggregate_exhaustion: {counters['agg_exhaustion']}")
    print(f"- bridge_crc_mismatch_logs: {counters['crc_mismatch']}")
    print(f"- bridge_uci_mismatch_logs: {counters['uci_mismatch']}")
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
    print(f"- dl_tx_data_bytes: {summary.get('tx_data_bytes', 0)}")
    if counters["rx_data"] == 0:
        print("- ul_userplane_throughput: unavailable from current logs (no RX_DATA indications logged)")
    else:
        ul_rx_mbps = summary.get("rx_data_bytes", 0) * 8.0 / args.duration / 1e6
        print(f"- ul_userplane_throughput_mbps: {ul_rx_mbps:.3f}")
    print()

    print("Interpretation")
    print("- raw_ul_bler_pct is based on Aerial's native CRC indications before OCUDU test-mode overrides.")
    print("- In OCUDU test mode without a real UE/RF waveform source, raw UL CRC failure is expected and does not by itself indicate FAPI integration failure.")
    print(f"- effective_test_mode_quality: {status_mark(all_clean)}")
    print("- effective_test_mode_quality reflects scheduler/runtime health: no slot jumps, no forced NACKs, no UCI discard, no bridge mismatch logs, no Aerial errors.")
    print()

    print("UCI Coverage")
    print(f"- pusch_uci_count: {summary.get('uci_pusch', len(uci_pusch_matches))}")
    print(f"- pucch01_uci_count: {summary.get('uci_pucch01', len(uci_pucch01_matches))}")
    print(f"- pucch234_uci_count: {summary.get('uci_pucch234', len(uci_pucch234_matches))}")
    print()

    print("Channel Signoff")
    print(f"- CONFIG/START: {observed_mark('CONFIG_RESPONSE' in gnb_stdout and 'START_REQUEST sent' in gnb_stdout)}")
    print(f"- SSB/PBCH: {observed_mark(False, ssb_configured)}")
    print(f"- PDCCH: {observed_mark(False, pdcch_configured)}")
    print(f"- PDSCH: {observed_mark(len(pdsch_matches) > 0)}")
    print(f"- PUSCH: {observed_mark(crc_total > 0 or summary.get('uci_pusch', len(uci_pusch_matches)) > 0)}")
    print(f"- PUCCH F0/F1: {observed_mark(summary.get('uci_pucch01', len(uci_pucch01_matches)) > 0)}")
    print(f"- PUCCH F2/F3/F4: {observed_mark(summary.get('uci_pucch234', len(uci_pucch234_matches)) > 0)}")
    print(f"- PRACH: {observed_mark(counters['rach_ind'] > 0, prach_configured)}")
    print(f"- SRS: {observed_mark(counters['srs_ind'] > 0, srs_configured)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
