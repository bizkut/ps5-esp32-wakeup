#!/usr/bin/env python3
"""Shared helpers for PS5 ESP32 Wake setup scripts."""

from __future__ import annotations

import glob
import ast
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

usb = None

SONY_VENDOR_ID = 0x054C
CONTROLLERS = {
    "dualshock3": (0x0268,),
    "dualshock4": (0x05C4, 0x09CC),
    "dualsense": (0x0CE6, 0x0DF2),
}
CONTROLLER_LABELS = {
    "dualshock3": "PS3 DualShock 3 / Sixaxis",
    "dualshock4": "PS4 DualShock 4",
    "dualsense": "PS5 DualSense",
}
CONFIG_KEYS = {
    "wifi_ssid": "CONFIG_PSWAKE_WIFI_SSID",
    "wifi_password": "CONFIG_PSWAKE_WIFI_PASSWORD",
    "profile_name": "CONFIG_PSWAKE_PROFILE_NAME",
    "token": "CONFIG_PSWAKE_TOKEN",
    "ps5_bt": "CONFIG_PSWAKE_PS5_BT_ADDR",
    "controller_bt": "CONFIG_PSWAKE_CONTROLLER_BT_ADDR",
    "udp_port": "CONFIG_PSWAKE_UDP_PORT",
    "ping_timeout_ms": "CONFIG_PSWAKE_PING_TIMEOUT_MS",
}
DEPRECATED_CONFIG_KEYS = (
    "CONFIG_PSWAKE_REST_RETURN_SETTLE_MS",
    "CONFIG_PSWAKE_LINUX_UP_TIMEOUT_MS",
)
MAC_RE = re.compile(r"^[0-9A-F]{2}(:[0-9A-F]{2}){5}$")
DEFAULT_TOKEN = "ps5-linux"
DEFAULT_UDP_PORT = 9755
DEFAULT_PING_TIMEOUT_MS = 1000
DEFAULT_PROFILE_NAME = "ps5"
DEFAULT_IDF_PATH = Path.home() / ".espressif" / "v6.0.1" / "esp-idf"

BOARD_PROFILES = {
    "tdisplay": {
        "label": "LilyGO/TTGO T-Display ESP32",
        "defaults": "profiles/t-display.defaults",
    },
    "devkitc": {
        "label": "ESP32-DevKitC without display",
        "defaults": "profiles/esp32-devkitc.defaults",
    },
}


def project_root_from_script() -> Path:
    return Path(__file__).resolve().parents[1]


def controller_cache_path(project: Path) -> Path:
    return project / ".pswake" / "controller.json"


def local_config_path(project: Path) -> Path:
    return project / "sdkconfig.local"


def parse_sdkconfig_value(value: str) -> str | int | bool:
    value = value.strip()
    if value == "y":
        return True
    if value == "n":
        return False
    if value.startswith('"') and value.endswith('"'):
        try:
            parsed = ast.literal_eval(value)
        except (SyntaxError, ValueError):
            parsed = value[1:-1]
        return str(parsed)
    try:
        return int(value, 0)
    except ValueError:
        return value


def load_sdkconfig_values(path: Path) -> dict[str, str | int | bool]:
    values: dict[str, str | int | bool] = {}
    if not path.exists():
        return values

    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        key, sep, value = line.partition("=")
        if sep:
            values[key] = parse_sdkconfig_value(value)
    return values


def mac_from_bytes(values: list[int]) -> str:
    mac = ":".join(f"{value:02X}" for value in values)
    if not MAC_RE.match(mac):
        raise RuntimeError(f"invalid Bluetooth address read from controller: {mac}")
    return mac


def read_feature_report(dev: Any, report_id: int, length: int) -> Any:
    report = dev.ctrl_transfer(0xA1, 0x01, report_id, 0x0000, length)
    if len(report) < length:
        raise RuntimeError(
            f"controller returned short report 0x{report_id:04X}: {len(report)} < {length}"
        )
    return report


def find_controller() -> tuple[str | None, Any, int | None]:
    global usb
    for controller_type, product_ids in CONTROLLERS.items():
        for product_id in product_ids:
            dev = usb.core.find(idVendor=SONY_VENDOR_ID, idProduct=product_id)
            if dev is not None:
                return controller_type, dev, product_id
    return None, None, None


def read_controller_addresses() -> dict[str, str]:
    global usb
    try:
        import usb.core
        import usb.util
    except ImportError:
        print(
            "error: pyusb is required. Install with: python3 -m pip install -r requirements.txt",
            file=sys.stderr,
        )
        raise SystemExit(2)

    controller_type, dev, product_id = find_controller()
    if dev is None:
        raise RuntimeError("no supported Sony controller found over USB")

    reattach = False
    try:
        try:
            if dev.is_kernel_driver_active(0):
                reattach = True
                dev.detach_kernel_driver(0)
        except (NotImplementedError, usb.core.USBError):
            pass

        usb.util.claim_interface(dev, 0)

        if controller_type == "dualshock3":
            controller_report = read_feature_report(dev, 0x03F2, 17)
            ps_report = read_feature_report(dev, 0x03F5, 8)
            controller_addr = mac_from_bytes([controller_report[i] for i in range(4, 10)])
            ps_addr = mac_from_bytes([ps_report[i] for i in range(2, 8)])
        elif controller_type in ("dualshock4", "dualsense"):
            report_id = 0x0312 if controller_type == "dualshock4" else 0x0309
            report = read_feature_report(dev, report_id, 20)
            controller_addr = mac_from_bytes([report[i] for i in (6, 5, 4, 3, 2, 1)])
            ps_addr = mac_from_bytes([report[i] for i in (15, 14, 13, 12, 11, 10)])
        else:
            raise RuntimeError(f"unsupported controller type: {controller_type}")
    finally:
        try:
            usb.util.release_interface(dev, 0)
        except usb.core.USBError:
            pass
        if reattach:
            try:
                dev.attach_kernel_driver(0)
            except usb.core.USBError:
                pass

    return {
        "controller_type": controller_type or "unknown",
        "controller_label": CONTROLLER_LABELS.get(controller_type or "", controller_type or "unknown"),
        "product_id": f"0x{product_id:04X}",
        "controller_bt": controller_addr,
        "ps5_bt": ps_addr,
    }


def save_controller_cache(project: Path, info: dict[str, str]) -> Path:
    cache = controller_cache_path(project)
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_text(json.dumps(info, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return cache


def load_controller_cache(project: Path) -> dict[str, str]:
    cache = controller_cache_path(project)
    if not cache.exists():
        raise RuntimeError(
            f"missing {cache}. Run ./tools/read_controller_addresses.py with a controller connected first."
        )
    data = json.loads(cache.read_text(encoding="utf-8"))
    for key in ("controller_bt", "ps5_bt"):
        value = str(data.get(key, "")).upper()
        if not MAC_RE.match(value):
            raise RuntimeError(f"{cache} contains invalid {key}: {value!r}")
        data[key] = value
    return data


def sdkconfig_assignment(key: str, value: str | int | bool) -> str:
    if isinstance(value, bool):
        return f"{key}={'y' if value else 'n'}"
    if isinstance(value, int):
        return f"{key}={value}"
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'{key}="{escaped}"'


def set_config_value(path: Path, key: str, value: str | int | bool) -> bool:
    assignment = sdkconfig_assignment(key, value)
    lines = path.read_text(encoding="utf-8").splitlines() if path.exists() else []
    changed = False
    replaced = False

    for index, line in enumerate(lines):
        if line.startswith(f"{key}="):
            replaced = True
            if line != assignment:
                lines[index] = assignment
                changed = True

    if not replaced:
        lines.append(assignment)
        changed = True

    if changed:
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return changed


def remove_config_values(path: Path, keys: tuple[str, ...]) -> bool:
    if not path.exists():
        return False
    lines = path.read_text(encoding="utf-8").splitlines()
    filtered = [
        line
        for line in lines
        if not any(line.startswith(f"{key}=") for key in keys)
    ]
    if filtered == lines:
        return False
    path.write_text("\n".join(filtered) + "\n", encoding="utf-8")
    return True


def write_local_config(project: Path, values: dict[str, str | int | bool]) -> Path:
    path = local_config_path(project)
    remove_config_values(path, DEPRECATED_CONFIG_KEYS)
    for key, value in values.items():
        set_config_value(path, key, value)
    return path


def update_generated_sdkconfig(project: Path, values: dict[str, str | int | bool]) -> bool:
    sdkconfig = project / "sdkconfig"
    if not sdkconfig.exists():
        return False
    changed = remove_config_values(sdkconfig, DEPRECATED_CONFIG_KEYS)
    for key, value in values.items():
        changed = set_config_value(sdkconfig, key, value) or changed
    return changed


def default_idf_py() -> str:
    path_idf = shutil.which("idf.py")
    if path_idf:
        return path_idf

    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        candidate = Path(idf_path) / "tools" / "idf.py"
        if candidate.exists():
            return str(candidate)

    return "idf.py"


def idf_export_script() -> Path | None:
    override = os.environ.get("PSWAKE_IDF_EXPORT")
    if override:
        path = Path(override).expanduser()
        return path if path.exists() else None

    idf_path = os.environ.get("IDF_PATH")
    candidates = []
    if idf_path:
        candidates.append(Path(idf_path) / "export.sh")
    candidates.extend(
        [
            DEFAULT_IDF_PATH / "export.sh",
            Path.home() / "esp" / "esp-idf" / "export.sh",
        ]
    )

    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def serial_ports() -> list[str]:
    patterns = (
        "/dev/cu.usbserial*",
        "/dev/cu.usbmodem*",
        "/dev/ttyUSB*",
        "/dev/ttyACM*",
    )
    ports: list[str] = []
    for pattern in patterns:
        ports.extend(glob.glob(pattern))
    return sorted(dict.fromkeys(ports))


def sdkconfig_defaults_arg(project: Path, board_key: str) -> str:
    profile = BOARD_PROFILES[board_key]["defaults"]
    parts = [
        "sdkconfig.defaults",
        profile,
        "sdkconfig.local",
    ]
    return ";".join(parts)


def idf_base_command(project: Path, board_key: str, idf_py: str) -> list[str]:
    return [
        idf_py,
        f"-DSDKCONFIG_DEFAULTS={sdkconfig_defaults_arg(project, board_key)}",
    ]


def run(command: list[str], cwd: Path) -> None:
    print("+ " + " ".join(command))
    subprocess.run(command, cwd=cwd, check=True)


def run_idf(command: list[str], cwd: Path) -> None:
    export = idf_export_script()
    if export is None:
        run(command, cwd)
        return

    quoted_command = " ".join(shlex.quote(part) for part in command)
    script = f". {shlex.quote(str(export))} >/dev/null && {quoted_command}"
    print(f"+ . {export} >/dev/null && {quoted_command}")
    subprocess.run(["/bin/zsh", "-lc", script], cwd=cwd, check=True)


def run_idf_action(project: Path, board_key: str, idf_py: str, action: str, port: str = "") -> None:
    base = idf_base_command(project, board_key, idf_py)
    run_idf([*base, "reconfigure"], project)

    if action in ("none", "configure"):
        return

    if action in ("build", "flash", "monitor"):
        run_idf([*base, "build"], project)

    if action in ("flash", "monitor"):
        flash_cmd = [*base]
        if port:
            flash_cmd.extend(["-p", port])
        flash_cmd.append("flash")
        run_idf(flash_cmd, project)

    if action == "monitor":
        monitor_cmd = [*base]
        if port:
            monitor_cmd.extend(["-p", port])
        monitor_cmd.append("monitor")
        run_idf(monitor_cmd, project)


def handle_common_errors(exc: Exception) -> int:
    if isinstance(exc, subprocess.CalledProcessError):
        return exc.returncode
    if isinstance(exc, RuntimeError):
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if exc.__class__.__name__ == "USBError":
        print(f"error: USB access failed: {exc}", file=sys.stderr)
        print(
            "Try another cable/port, close apps using the controller, or run with USB permissions.",
            file=sys.stderr,
        )
        return 1
    raise exc
