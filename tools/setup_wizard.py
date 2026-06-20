#!/usr/bin/env python3
"""Interactive ESP32 setup wizard for PS5 ESP32 Wake."""

from __future__ import annotations

import argparse
import getpass
from pathlib import Path

from pswake_helpers import (
    BOARD_PROFILES,
    CONFIG_KEYS,
    DEFAULT_PROFILE_NAME,
    DEFAULT_TOKEN,
    DEFAULT_UDP_PORT,
    default_idf_py,
    handle_common_errors,
    load_sdkconfig_values,
    load_controller_cache,
    local_config_path,
    project_root_from_script,
    run_idf_action,
    serial_ports,
    update_generated_sdkconfig,
    write_local_config,
)


ACTION_CHOICES = ("configure", "build", "flash", "monitor")


def ask(prompt: str, default: str = "") -> str:
    suffix = f" [{default}]" if default else ""
    value = input(f"{prompt}{suffix}: ").strip()
    return value if value else default


def ask_secret(prompt: str, default_present: bool = False) -> str:
    suffix = " [keep blank]" if default_present else ""
    return getpass.getpass(f"{prompt}{suffix}: ")


def ask_choice(prompt: str, choices: list[tuple[str, str]], default: str) -> str:
    default_index = next(
        (index for index, (key, _) in enumerate(choices, start=1) if key == default),
        1,
    )
    print(prompt)
    for index, (key, label) in enumerate(choices, start=1):
        marker = " (default)" if key == default else ""
        print(f"  {index}. {label}{marker}")

    while True:
        value = input(f"Select [{default_index}]: ").strip()
        if not value:
            return default
        if value in {key for key, _ in choices}:
            return value
        if value.isdigit():
            index = int(value)
            if 1 <= index <= len(choices):
                return choices[index - 1][0]
        print("Enter a number from the list.")


def default_serial_port() -> str:
    ports = serial_ports()
    if not ports:
        return ""
    for port in ports:
        if "usbserial" in port or "usbmodem" in port:
            return port
    return ports[0]


def config_str(
    existing: dict[str, str | int | bool],
    key: str,
    default: str = "",
) -> str:
    value = existing.get(key, default)
    return str(value)


def config_int(
    existing: dict[str, str | int | bool],
    key: str,
    default: int,
) -> int:
    value = existing.get(key, default)
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Write private ESP32 config and optionally build/flash PS5 ESP32 Wake."
    )
    parser.add_argument("--project", type=Path, default=project_root_from_script())
    parser.add_argument("--idf-py", default=default_idf_py(), help="Path to idf.py")
    parser.add_argument("--board", choices=sorted(BOARD_PROFILES), help="Board profile")
    parser.add_argument("--ssid", help="Wi-Fi SSID")
    parser.add_argument("--password", help="Wi-Fi password")
    parser.add_argument("--token", default=None, help="Linux loader beacon token")
    parser.add_argument("--profile-name", default=None, help="Firmware profile name")
    parser.add_argument("--udp-port", type=int, default=None, help="UDP beacon listen port")
    parser.add_argument("--port", default=None, help="ESP32 serial port")
    parser.add_argument("--action", choices=ACTION_CHOICES, default=None)
    parser.add_argument("--non-interactive", action="store_true")
    args = parser.parse_args()

    project = args.project.resolve()
    cache = load_controller_cache(project)
    existing_config = load_sdkconfig_values(local_config_path(project))

    print("Cached controller data")
    print(f"  controller: {cache.get('controller_label', cache.get('controller_type', 'unknown'))}")
    print(f"  controller BT: {cache['controller_bt']}")
    print(f"  paired PlayStation BT: {cache['ps5_bt']}")
    print()

    board_choices = [(key, data["label"]) for key, data in BOARD_PROFILES.items()]
    if args.non_interactive:
        board = args.board or "tdisplay"
        ssid = args.ssid if args.ssid is not None else config_str(
            existing_config, CONFIG_KEYS["wifi_ssid"]
        )
        password = args.password if args.password is not None else config_str(
            existing_config, CONFIG_KEYS["wifi_password"]
        )
        token = args.token if args.token is not None else config_str(
            existing_config, CONFIG_KEYS["token"], DEFAULT_TOKEN
        )
        profile_name = args.profile_name if args.profile_name is not None else config_str(
            existing_config, CONFIG_KEYS["profile_name"], DEFAULT_PROFILE_NAME
        )
        udp_port = args.udp_port if args.udp_port is not None else config_int(
            existing_config, CONFIG_KEYS["udp_port"], DEFAULT_UDP_PORT
        )
        port = args.port or default_serial_port()
        action = args.action or "configure"
    else:
        board = args.board or ask_choice("Board profile", board_choices, "tdisplay")
        ssid_default = config_str(existing_config, CONFIG_KEYS["wifi_ssid"])
        password_default = config_str(existing_config, CONFIG_KEYS["wifi_password"])
        ssid = args.ssid if args.ssid is not None else ask("Wi-Fi SSID", ssid_default)
        if args.password is not None:
            password = args.password
        else:
            password_input = ask_secret("Wi-Fi password", bool(password_default))
            password = password_input if password_input else password_default
        token_default = config_str(existing_config, CONFIG_KEYS["token"], DEFAULT_TOKEN)
        token = args.token or ask(
            "Linux loader token (must match LINUX_WAKE_BEACON_TOKEN)",
            token_default,
        )
        profile_default = config_str(
            existing_config, CONFIG_KEYS["profile_name"], DEFAULT_PROFILE_NAME
        )
        profile_name = args.profile_name or ask("Firmware profile name", profile_default)
        udp_default = str(
            args.udp_port if args.udp_port is not None else
            config_int(existing_config, CONFIG_KEYS["udp_port"], DEFAULT_UDP_PORT)
        )
        udp_port = int(ask("UDP beacon port", udp_default))
        port_default = args.port if args.port is not None else default_serial_port()
        port = ask("ESP32 serial port", port_default)
        action = args.action or ask_choice(
            "Action",
            [
                ("configure", "configure only"),
                ("build", "configure and build"),
                ("flash", "configure, build, and flash"),
                ("monitor", "configure, build, flash, and monitor"),
            ],
            "flash",
        )

    config_values = {
        CONFIG_KEYS["wifi_ssid"]: ssid,
        CONFIG_KEYS["wifi_password"]: password,
        CONFIG_KEYS["profile_name"]: profile_name,
        CONFIG_KEYS["token"]: token,
        CONFIG_KEYS["ps5_bt"]: cache["ps5_bt"],
        CONFIG_KEYS["controller_bt"]: cache["controller_bt"],
        CONFIG_KEYS["udp_port"]: udp_port,
    }

    config = write_local_config(project, config_values)
    changed_sdkconfig = update_generated_sdkconfig(project, config_values)

    print(f"wrote: {config}")
    if changed_sdkconfig:
        print(f"updated ignored generated config: {project / 'sdkconfig'}")
    print(f"board: {BOARD_PROFILES[board]['label']}")
    print(f"wifi: {ssid if ssid else 'not configured; AP fallback will be used'}")
    print(f"token: {token}")

    run_idf_action(project, board, args.idf_py, action, port)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
    except Exception as exc:
        raise SystemExit(handle_common_errors(exc))
