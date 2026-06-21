# Technical Details

## Repository Contents

- `main/` - ESP32 firmware source.
- `tools/read_controller_addresses.py` - USB helper that reads the paired
  controller and PlayStation Bluetooth addresses into `.pswake/controller.json`.
- `tools/setup_wizard.py` - setup wizard that writes private settings into
  ignored `sdkconfig.local`.
- `tools/configure_from_controller.py` - compatibility one-shot helper.
- `tools/generate_linux_logo_alpha.py` - regenerates the anti-aliased Linux LCD
  logo asset from `ps5-linux.jpg`.
- `ps5-linux-loader-udp-beacon.patch` - reference patch for compatible PS5 Linux
  loader trees.
- `profiles/` - board defaults for T-Display and headless DevKitC.

## Device Profiles

Available board profiles:

- `LilyGO/TTGO T-Display ESP32` - enables ST7789 LCD, PS5/Linux logos, and
  onboard wake/cancel buttons.
- `ESP32-DevKitC without display` - disables LCD and button GPIO handling while
  keeping Wi-Fi, UDP beacon detection, ping monitoring, and Classic Bluetooth
  wake.

The default profile is T-Display. You can also configure manually:

```sh
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;profiles/t-display.defaults" reconfigure
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;profiles/esp32-devkitc.defaults" reconfigure
```

## Compatible Controllers

The USB helper can read paired Bluetooth addresses from:

- PS5 DualSense
- PS4 DualShock 4
- PS3 DualShock 3 / Sixaxis

## Loader Beacon

The easiest path is the modified Linux loader already included with:

```text
https://github.com/bizkut/ps5-umtx2-host
```

The reference patch in this repository sends UDP broadcasts before rest mode:

```text
PS5LINUX_ARMED v1 token=ps5-linux fw=<firmware>
```

Apply it manually from a compatible PS5 Linux loader checkout with:

```sh
patch -p1 < /path/to/ps5-esp32-wakeup/ps5-linux-loader-udp-beacon.patch
```

## Manual ESP-IDF Use

If you do not use the helper scripts, source ESP-IDF first:

```sh
. "$HOME/.espressif/v6.0.1/esp-idf/export.sh"
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

On macOS, the serial port is usually `/dev/cu.usbserial-*` or
`/dev/cu.usbmodem*`.

## Local Private Files

The helper writes private values to ignored files:

- `.pswake/controller.json` - controller and paired PlayStation Bluetooth
  addresses.
- `sdkconfig.local` - Wi-Fi, token, board profile, UDP port, and Bluetooth
  addresses.

Do not commit generated local config. `sdkconfig`, `sdkconfig.local`, and
`.pswake/` can contain Wi-Fi credentials and device-specific Bluetooth
addresses.

## Display Notes

For T-Display, the LCD settings target a `240 x 135` ST7789 panel in portrait
layout. If the image is rotated or offset on your board revision, adjust these
settings in `menuconfig`:

- `Swap LCD X/Y axes`
- `Mirror LCD X axis`
- `Mirror LCD Y axis`
- `LCD X gap/offset`
- `LCD Y gap/offset`
- `T-Display LCD backlight brightness percent`

The Linux logo is generated as a final-size anti-aliased alpha asset:

```sh
python3 tools/generate_linux_logo_alpha.py
```

## Bluetooth Wake Notes

The firmware sets the ESP32 BT interface MAC to the paired controller address
before enabling the Classic BT controller, then sends the HCI Create Connection
packet used by PlayStation Bluetooth wake tools.

If the ESP32 controller does not use the overridden BT MAC for Classic HCI
paging, the UI and serial log will show the HCI failure state.

## Acknowledgements

Bluetooth wake behavior and USB controller address extraction are based on the
Apache-2.0 licensed upstream `pywakepsXonbt` project:

```text
https://github.com/FreeTHX/pywakepsXonbt
```
