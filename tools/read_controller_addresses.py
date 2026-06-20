#!/usr/bin/env python3
"""Read controller and paired PlayStation Bluetooth addresses into a local cache."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from pswake_helpers import (
    handle_common_errors,
    project_root_from_script,
    read_controller_addresses,
    save_controller_cache,
)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Read paired Bluetooth addresses from a USB-connected PS3/PS4/PS5 controller."
    )
    parser.add_argument("--project", type=Path, default=project_root_from_script())
    args = parser.parse_args()

    project = args.project.resolve()
    info = read_controller_addresses()
    cache = save_controller_cache(project, info)

    print(f"controller: {info['controller_label']} ({info['product_id']})")
    print(f"controller BT: {info['controller_bt']}")
    print(f"paired PlayStation BT: {info['ps5_bt']}")
    print(f"saved: {cache}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        raise SystemExit(handle_common_errors(exc))
