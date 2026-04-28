#!/usr/bin/env python3
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Generate a standalone 100 MHz Aerial loopback config from P5G_GH."""

from pathlib import Path
import sys

import yaml


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: make_loopback_config.py <source_yaml> <dest_yaml>", file=sys.stderr)
        return 2

    source = Path(sys.argv[1])
    dest = Path(sys.argv[2])

    data = yaml.safe_load(source.read_text())
    cfg = data["cuphydriver_config"]

    data["l2adapter_filename"] = "l2_adapter_config_loopback.yaml"
    data["standalone_filename"] = "launch_pattern.yaml"
    data["aerial_metrics_backend_address"] = "127.0.0.1:8081"
    data["low_priority_core"] = 4

    cfg["standalone"] = 1
    cfg["validation"] = 0
    cfg["fh_stats_dump_cpu_core"] = -1
    cfg["workers_ul"] = [2, 3]
    cfg["workers_dl"] = [4, 5, 6]
    cfg["gpu_init_comms_dl"] = 0
    cfg["enable_h2d_copy_thread"] = 0

    # Standalone still parses the cell NIC field, so preserve the real PCI address
    # from the source profile while collapsing the config to a single 100 MHz cell.
    if cfg.get("nics"):
        cfg["nics"] = [dict(cfg["nics"][0])]

    if cfg.get("cells"):
        cell = dict(cfg["cells"][0])
        cell["name"] = "Loopback RU 0"
        cell["cell_id"] = 1
        cell["src_mac_addr"] = "00:00:00:00:00:00"
        cell["dst_mac_addr"] = "00:00:00:00:00:00"
        cell["vlan"] = 0
        cell["pcp"] = 0
        cfg["cells"] = [cell]

    dest.write_text(yaml.safe_dump(data, sort_keys=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
