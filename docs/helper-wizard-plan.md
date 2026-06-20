# Helper Script Wizard Plan

## Summary

Refactor the helper flow into two scripts:

- `tools/read_controller_addresses.py`: run first with the PS controller connected over USB; reads controller BT MAC and paired PlayStation BT MAC, then saves them to an ignored local cache.
- `tools/setup_wizard.py`: run after connecting the ESP32; interactively collects board profile, Wi-Fi SSID/password, beacon token, serial port, and build/flash choice.

Private values must never be written to tracked `sdkconfig.defaults`.

## Key Changes

- Add ignored local config files:
  - `.pswake/controller.json` for controller/PlayStation BT addresses.
  - `sdkconfig.local` for private ESP-IDF config values such as Wi-Fi and BT MACs.
- Update `.gitignore` to ignore `.pswake/` and `sdkconfig.local`.
- Keep public defaults generic in `sdkconfig.defaults`.
- Preserve `tools/configure_from_controller.py` as a compatibility wrapper or deprecation shim pointing users to the new two-step flow.

## Defaults And Wizard Behavior

- Use these wizard defaults:
  - board profile: `LilyGO/TTGO T-Display ESP32`
  - beacon token: `ps5-linux`
  - UDP port: `9755`
  - profile name: `ps5`
  - action: `flash`
- The token prompt must explain that it must match the Linux loader token:
  - loader macro: `LINUX_WAKE_BEACON_TOKEN`
  - current loader patch default: `"ps5-linux"`
  - beacon example: `PS5LINUX_ARMED v1 token=ps5-linux fw=<firmware>`
- `read_controller_addresses.py`
  - Detects PS5 DualSense, PS4 DualShock 4, or PS3 DualShock 3/Sixaxis over USB.
  - Prints detected controller type, controller BT address, and paired PlayStation BT address.
  - Saves the result to `.pswake/controller.json`.
  - Fails clearly if `pyusb` is missing or no supported controller is connected.
- `setup_wizard.py`
  - Loads cached BT addresses from `.pswake/controller.json`.
  - Prompts for board profile, Wi-Fi SSID, hidden Wi-Fi password, token, serial port, and action.
  - Writes `sdkconfig.local`.
  - Runs ESP-IDF with `sdkconfig.defaults`, selected profile defaults, and `sdkconfig.local`.
  - Supports configure-only, build, flash, and flash+monitor.

## Documentation

Update README to show the new workflow:

```sh
python3 -m pip install -r requirements.txt
./tools/read_controller_addresses.py

# unplug controller, plug ESP32
./tools/setup_wizard.py
```

Document that the default token is `ps5-linux` and must stay in sync with `LINUX_WAKE_BEACON_TOKEN` in the Linux loader patch.

## Test Plan

- Run Python syntax checks:
  - `python3 -m py_compile tools/read_controller_addresses.py tools/setup_wizard.py tools/configure_from_controller.py`
- Verify `--help` output for all helper scripts.
- Run wizard with temporary fake/cached address data and confirm it writes only ignored local files.
- Confirm no secrets are written to tracked files with `git diff`.
- Build both profiles:
  - T-Display profile
  - ESP32-DevKitC headless profile
- If ESP32 is connected, flash using the wizard-selected serial port.

## Assumptions

- Wizard prompts stay essential only: board profile, Wi-Fi, token, port, and action.
- Advanced timeout/LCD/AP settings remain available through `idf.py menuconfig`.
- Local private config is intentionally ignored and must not be committed.
