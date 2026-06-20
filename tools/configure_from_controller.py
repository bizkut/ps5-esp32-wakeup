#!/usr/bin/env python3
"""Compatibility helper: read controller addresses and configure ESP-IDF locally."""

from __future__ import annotations

import argparse
from pathlib import Path

from pswake_helpers import (
    BOARD_PROFILES,
    CONFIG_KEYS,
    DEFAULT_PROFILE_NAME,
    DEFAULT_TOKEN,
    DEFAULT_UDP_PORT,
    default_idf_py,
    handle_common_errors,
    read_controller_addresses,
    run_idf_action,
    save_controller_cache,
    update_generated_sdkconfig,
    write_local_config,
)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Read paired controller/PlayStation BT addresses from USB and write ignored local ESP32 config. "
            "For the guided flow, use read_controller_addresses.py then setup_wizard.py."
        )
    )
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--skip-reconfigure", action="store_true")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--flash", action="store_true")
    parser.add_argument("--port", help="Serial port for idf.py flash, for example /dev/ttyUSB0")
    parser.add_argument("--idf-py", default=default_idf_py(), help="Path to idf.py")
    parser.add_argument("--board", choices=sorted(BOARD_PROFILES), default="tdisplay")
    parser.add_argument("--ssid", default="", help="Wi-Fi SSID to write into sdkconfig.local")
    parser.add_argument("--password", default="", help="Wi-Fi password to write into sdkconfig.local")
    parser.add_argument("--token", default=DEFAULT_TOKEN, help="Linux loader beacon token")
    parser.add_argument("--profile-name", default=DEFAULT_PROFILE_NAME)
    parser.add_argument("--udp-port", type=int, default=DEFAULT_UDP_PORT)
    args = parser.parse_args()

    project = args.project.resolve()
    info = read_controller_addresses()
    cache = save_controller_cache(project, info)

    print(f"controller: {info['controller_label']} ({info['product_id']})")
    print(f"controller BT: {info['controller_bt']}")
    print(f"paired PlayStation BT: {info['ps5_bt']}")
    print(f"saved: {cache}")

    config_values = {
        CONFIG_KEYS["wifi_ssid"]: args.ssid,
        CONFIG_KEYS["wifi_password"]: args.password,
        CONFIG_KEYS["profile_name"]: args.profile_name,
        CONFIG_KEYS["token"]: args.token,
        CONFIG_KEYS["ps5_bt"]: info["ps5_bt"],
        CONFIG_KEYS["controller_bt"]: info["controller_bt"],
        CONFIG_KEYS["udp_port"]: args.udp_port,
    }
    config = write_local_config(project, config_values)
    changed_sdkconfig = update_generated_sdkconfig(project, config_values)

    print(f"wrote: {config}")
    if changed_sdkconfig:
        print(f"updated ignored generated config: {project / 'sdkconfig'}")

    if not args.skip_reconfigure:
        action = "configure"
        if args.flash:
            action = "flash"
        elif args.build:
            action = "build"
        run_idf_action(project, args.board, args.idf_py, action, args.port or "")
    elif args.build or args.flash:
        action = "flash" if args.flash else "build"
        run_idf_action(project, args.board, args.idf_py, action, args.port or "")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        raise SystemExit(handle_common_errors(exc))
