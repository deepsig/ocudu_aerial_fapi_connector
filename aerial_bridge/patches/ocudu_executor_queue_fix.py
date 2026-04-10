#!/usr/bin/env python3
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
from pathlib import Path
import sys


def patch(path_str: str) -> None:
    path = Path(path_str)
    text = path.read_text()
    old = "    concurrent_queue_params slot_qparams{concurrent_queue_policy::lockfree_spsc, 4};\n"
    new = "    concurrent_queue_params slot_qparams{concurrent_queue_policy::lockfree_spsc, 64};\n"
    if old not in text:
      if new in text:
        return
      raise SystemExit(f"pattern not found in {path}")
    path.write_text(text.replace(old, new, 1))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: ocudu_executor_queue_fix.py <du_high_executor_mapper.cpp>")
    patch(sys.argv[1])
