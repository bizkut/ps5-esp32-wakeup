#!/usr/bin/env python3
"""Read paired PlayStation BT addresses from a USB controller and update ESP-IDF config."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

usb = None


SONY_VENDOR_ID = 0x054C
CONTROLLERS = {
    "dualshock3": (0x0268,),
    "dualshock4": (0x05C4, 0x09CC),
    "dualsense": (0x0CE6, 0x0DF2),
}
CONFIG_KEYS = {
    "ps5": "CONFIG_PSWAKE_PS5_BT_ADDR",
    "controller": "CONFIG_PSWAKE_CONTROLLER_BT_ADDR",
}
MAC_RE = re.compile(r"^[0-9A-F]{2}(:[0-9A-F]{2}){5}$")


def mac_from_bytes(values: list[int]) -> str:
    mac = ":".join(f"{value:02X}" for value in values)
    if not MAC_RE.match(mac):
        raise RuntimeError(f"invalid Bluetooth address read from controller: {mac}")
    return mac


def read_feature_report(dev, report_id: int, length: int):
    report = dev.ctrl_transfer(0xA1, 0x01, report_id, 0x0000, length)
    if len(report) < length:
        raise RuntimeError(
            f"controller returned short report 0x{report_id:04X}: {len(report)} < {length}"
        )
    return report


def find_controller():
    global usb
    for controller_type, product_ids in CONTROLLERS.items():
        for product_id in product_ids:
            dev = usb.core.find(idVendor=SONY_VENDOR_ID, idProduct=product_id)
            if dev is not None:
                return controller_type, dev, product_id
    return None, None, None


def read_addresses():
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
            ps5_addr = mac_from_bytes([ps_report[i] for i in range(2, 8)])
        elif controller_type in ("dualshock4", "dualsense"):
            report_id = 0x0312 if controller_type == "dualshock4" else 0x0309
            report = read_feature_report(dev, report_id, 20)
            controller_addr = mac_from_bytes([report[i] for i in (6, 5, 4, 3, 2, 1)])
            ps5_addr = mac_from_bytes([report[i] for i in (15, 14, 13, 12, 11, 10)])
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
        "type": controller_type,
        "product_id": f"0x{product_id:04X}",
        "controller": controller_addr,
        "ps5": ps5_addr,
    }


def set_config_value(path: Path, key: str, value: str) -> bool:
    assignment = f'{key}="{value}"'
    if path.exists():
        lines = path.read_text(encoding="utf-8").splitlines()
    else:
        lines = []

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


def run(command: list[str], cwd: Path) -> None:
    print("+ " + " ".join(command))
    subprocess.run(command, cwd=cwd, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Pull paired controller/PlayStation BT addresses from a USB controller into ESP32 config."
    )
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--skip-reconfigure", action="store_true")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--flash", action="store_true")
    parser.add_argument("--port", help="Serial port for idf.py flash, for example /dev/ttyUSB0")
    parser.add_argument("--idf-py", default=default_idf_py(), help="Path to idf.py")
    args = parser.parse_args()

    project = args.project.resolve()
    defaults = project / "sdkconfig.defaults"
    sdkconfig = project / "sdkconfig"

    info = read_addresses()
    print(f"controller: {info['type']} ({info['product_id']})")
    print(f"controller BT: {info['controller']}")
    print(f"paired PS BT: {info['ps5']}")

    changed_defaults = set_config_value(defaults, CONFIG_KEYS["controller"], info["controller"])
    changed_defaults |= set_config_value(defaults, CONFIG_KEYS["ps5"], info["ps5"])

    changed_sdkconfig = False
    if sdkconfig.exists():
        changed_sdkconfig = set_config_value(sdkconfig, CONFIG_KEYS["controller"], info["controller"])
        changed_sdkconfig |= set_config_value(sdkconfig, CONFIG_KEYS["ps5"], info["ps5"])

    if changed_defaults:
        print(f"updated {defaults}")
    else:
        print(f"{defaults} already matches")

    if sdkconfig.exists():
        if changed_sdkconfig:
            print(f"updated {sdkconfig}")
        else:
            print(f"{sdkconfig} already matches")

    if not args.skip_reconfigure:
        run([args.idf_py, "reconfigure"], project)

    if args.build or args.flash:
        run([args.idf_py, "build"], project)

    if args.flash:
        command = [args.idf_py]
        if args.port:
            command.extend(["-p", args.port])
        command.append("flash")
        run(command, project)

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        raise SystemExit(exc.returncode)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
    except Exception as exc:
        if exc.__class__.__name__ != "USBError":
            raise
        print(f"error: USB access failed: {exc}", file=sys.stderr)
        print("Try another cable/port, close apps using the controller, or run with USB permissions.", file=sys.stderr)
        raise SystemExit(1)
