# PS5 ESP32 Wake

ESP-IDF firmware for ESP32 boards that listens for a PS5 Linux loader UDP
beacon, watches the console until it enters rest mode, and then attempts a
Classic Bluetooth wake using the paired controller and PlayStation Bluetooth
addresses.

The LCD shows the PS5 logo by default. When the loader beacon is detected, the
screen says `LINUX TIME`. The rest of the automatic flow is ping-driven:
`WAIT REST`, `WAKING UP`, `LINUX BOOT`, `LINUX ONLINE`, `LINUX OFFLINE`, and
back to `READY`.

This is an unofficial hobby project and is not affiliated with or endorsed by
Sony Interactive Entertainment.

## Repository Contents

- `main/` - ESP32 firmware source.
- `tools/read_controller_addresses.py` - USB helper that reads the paired
  controller and PlayStation Bluetooth addresses into `.pswake/controller.json`.
- `tools/setup_wizard.py` - interactive ESP32 setup wizard that writes private
  settings into ignored `sdkconfig.local`.
- `tools/configure_from_controller.py` - compatibility one-shot helper.
- `ps5-linux-loader-udp-beacon.patch` - reference patch for a compatible PS5
  Linux loader tree to broadcast the arming beacon before rest mode.
- `ps5-logo.jpg`, `ps5-linux.jpg` - source images used to generate the embedded
  LCD logo masks.
- `profiles/` - board profile defaults for T-Display and headless DevKitC.

## Device Profiles

Two ESP32 board profiles are available:

- `LilyGO/TTGO T-Display ESP32` - enables the ST7789 LCD, PS5/Linux logos, and
  the onboard wake/cancel buttons.
- `ESP32-DevKitC without display` - disables LCD and button GPIO handling for a
  headless board while keeping Wi-Fi, UDP beacon detection, ping monitoring, and
  Classic Bluetooth wake.

The default profile is T-Display. Select the board in `idf.py menuconfig` under
`PS5 Linux Wake > Device profile`, or use one of the profile defaults:

```sh
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;profiles/t-display.defaults" reconfigure
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;profiles/esp32-devkitc.defaults" reconfigure
```

T-Display ESP32 boards are commonly available for about USD $8. One example:
https://www.aliexpress.com/item/1005006860890245.html

## Manual Wake Button

On the T-Display profile, GPIO0 is configured as a manual Bluetooth wake button.
Pressing it sends the same paired-controller Bluetooth wake sequence used by the
automatic Linux wake flow and shows `WAKING UP` on the LCD. It can be used any
time after boot, even before a Linux loader beacon has been detected.

## Compatible Controllers

The USB configuration helper can read paired Bluetooth addresses from these
Sony controllers:

- PS5 DualSense
- PS4 DualShock 4
- PS3 DualShock 3 / Sixaxis

## Patched Linux Loader

The easiest path is to use the modified Linux loader that is already included
with `ps5-umtx2-host`:

```text
https://github.com/bizkut/ps5-umtx2-host
```

Inject the Linux loader payload from that host project. It already includes the
UDP beacon used by this ESP32 firmware.

The patch file in this repository is kept as a reference for compatible loader
trees. To apply it manually from a PS5 Linux loader checkout:

```sh
patch -p1 < /path/to/ps5-esp32-wakeup/ps5-linux-loader-udp-beacon.patch
```

The patch sends three UDP broadcasts before `enter_rest_mode()`:

```text
PS5LINUX_ARMED v1 token=ps5-linux fw=<firmware>
```

## Install ESP-IDF

Install Espressif's ESP-IDF before building or flashing:

```sh
mkdir -p "$HOME/.espressif/v6.0.1"
cd "$HOME/.espressif/v6.0.1"
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32
. ./export.sh
```

After install, confirm `idf.py` works:

```sh
idf.py --version
```

The helper scripts automatically source
`$HOME/.espressif/v6.0.1/esp-idf/export.sh` when running `idf.py`. If your
ESP-IDF checkout lives somewhere else, source it manually before running the
helper, or set:

```sh
export IDF_PATH=/path/to/esp-idf
```

## Configure Firmware

If you are not using the helper scripts, source the ESP-IDF environment in your
shell:

```sh
. "$HOME/.espressif/v6.0.1/esp-idf/export.sh"
```

Use `menuconfig` for board profile, Wi-Fi, token, timeout, and display settings:

```sh
idf.py set-target esp32
idf.py menuconfig
```

For T-Display, the default LCD settings target a 240x135 display in landscape
mode. If the image is rotated or offset on your board revision, adjust
`Swap LCD X/Y axes`, `Mirror LCD X axis`, `Mirror LCD Y axis`,
`LCD X gap/offset`, and `LCD Y gap/offset` in `menuconfig`.

To pull the paired controller and PlayStation Bluetooth addresses directly from
a compatible USB-connected controller, then configure the ESP32:

```sh
python3 -m pip install -r requirements.txt

# first: controller connected over USB
./tools/read_controller_addresses.py

# second: unplug the controller, plug in the ESP32
./tools/setup_wizard.py
```

The wizard writes private values to ignored local files:

- `.pswake/controller.json` - controller and paired PlayStation BT addresses.
- `sdkconfig.local` - Wi-Fi, token, profile, UDP port, and BT addresses.

The default Linux loader token is `ps5-linux`. It must match the loader payload
you inject, including the ready-to-use loader in
`https://github.com/bizkut/ps5-umtx2-host`. The default beacon looks like:

```text
PS5LINUX_ARMED v1 token=ps5-linux fw=<firmware>
```

The compatibility helper still supports the older one-shot flow without writing
private values to tracked `sdkconfig.defaults`:

```sh
./tools/configure_from_controller.py --build
./tools/configure_from_controller.py --flash --port /dev/ttyUSB0
```

If `idf.py` is not on `PATH`, pass it explicitly:

```sh
./tools/configure_from_controller.py --idf-py "$IDF_PATH/tools/idf.py" --build
```

Do not commit generated local config; `sdkconfig`, `sdkconfig.local`, and
`.pswake/` can contain Wi-Fi credentials and device-specific Bluetooth
addresses.

## Build

```sh
idf.py set-target esp32
idf.py build
```

## Flash

```sh
idf.py -p /dev/ttyUSB0 flash monitor
```

On macOS, the serial port is usually named like `/dev/cu.usbserial-*` or
`/dev/cu.usbmodem*`.

## Bluetooth Wake Notes

The firmware sets the ESP32 BT interface MAC to the paired controller address
before enabling the Classic BT controller, then sends the HCI Create Connection
packet used by PlayStation Bluetooth wake tools.

This still needs hardware validation against each ESP32 board/PS5 combination.
If the ESP32 controller does not actually use the overridden BT MAC for Classic
HCI paging, the UI and serial log will show the HCI failure state.

## Acknowledgements

The Bluetooth wake approach and USB controller address extraction are based on
the Apache-2.0 licensed upstream `pywakepsXonbt` project:

https://github.com/FreeTHX/pywakepsXonbt
