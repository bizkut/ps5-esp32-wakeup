# PS5 ESP32 Wake

ESP-IDF firmware for a LilyGO/TTGO T-Display ESP32-WROOM that listens for a
PS5 Linux loader UDP beacon, watches the console until it enters rest mode, and
then attempts a Classic Bluetooth wake using the paired controller and
PlayStation Bluetooth addresses.

The LCD shows the PS5 logo by default. When the loader beacon is detected, the
screen says `LINUX TIME`. After Linux confirms boot with a `PS5LINUX_UP` packet,
the display switches to the Linux logo.

This is an unofficial hobby project and is not affiliated with or endorsed by
Sony Interactive Entertainment.

## Repository Contents

- `main/` - ESP32 firmware source.
- `tools/configure_from_controller.py` - USB helper that reads the paired
  controller and PlayStation Bluetooth addresses into ESP-IDF config.
- `ps5-linux-loader-udp-beacon.patch` - patch for a compatible PS5 Linux loader
  tree to broadcast the arming beacon before rest mode.
- `ps5-logo.jpg`, `ps5-linux.jpg` - source images used to generate the embedded
  LCD logo masks.

## Loader Patch

From your PS5 Linux loader checkout:

```sh
patch -p1 < /path/to/ps5-esp32-wakeup/ps5-linux-loader-udp-beacon.patch
```

The patch sends three UDP broadcasts before `enter_rest_mode()`:

```text
PS5LINUX_ARMED v1 token=ps5-linux fw=<firmware>
```

## Configure Firmware

Install ESP-IDF, then source its environment in your shell:

```sh
. "$HOME/esp/esp-idf/export.sh"
```

Use `menuconfig` for Wi-Fi, token, timeout, and display settings:

```sh
idf.py set-target esp32
idf.py menuconfig
```

To pull the paired controller and PlayStation Bluetooth addresses directly from
a USB-connected DualSense, DualShock 4, or DualShock 3 controller:

```sh
python3 -m pip install -r requirements.txt
./tools/configure_from_controller.py
```

The helper updates `sdkconfig.defaults` and the generated `sdkconfig` when it
exists, then runs `idf.py reconfigure`. To continue straight into a build or
flash:

```sh
./tools/configure_from_controller.py --build
./tools/configure_from_controller.py --flash --port /dev/ttyUSB0
```

If `idf.py` is not on `PATH`, pass it explicitly:

```sh
./tools/configure_from_controller.py --idf-py "$IDF_PATH/tools/idf.py" --build
```

Do not commit your generated `sdkconfig`; it can contain local Wi-Fi credentials
and device-specific Bluetooth addresses.

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
