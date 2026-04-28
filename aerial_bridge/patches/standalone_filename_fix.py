#!/usr/bin/env python3
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from pathlib import Path
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        if new in text:
            return text
        raise SystemExit(f"{label}: pattern not found")
    return text.replace(old, new, 1)


def patch(path_str: str) -> None:
    path = Path(path_str)
    text = path.read_text()

    text = replace_once(
        text,
        "        if(parser_l2pattern.parse_launch_pattern_file(parser_cfg.get_config_filename().c_str()))\n",
        "        if(parser_l2pattern.parse_launch_pattern_file(parser_cfg.get_standalone_filename().c_str()))\n",
        "standalone launch pattern path",
    )
    text = replace_once(
        text,
        '            NVLOGE_FMT(TAG, AERIAL_YAML_PARSER_EVENT, "Error parsing standalone config file {}", parser_cfg.get_config_filename().c_str());\n',
        '            NVLOGE_FMT(TAG, AERIAL_YAML_PARSER_EVENT, "Error parsing standalone config file {}", parser_cfg.get_standalone_filename().c_str());\n',
        "standalone launch pattern log path",
    )

    path.write_text(text)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: standalone_filename_fix.py <cuphycontroller_scf.cpp>")
    patch(sys.argv[1])
