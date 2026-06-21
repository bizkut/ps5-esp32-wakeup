# PS5 ESP32 Wake

ESP32 firmware that wakes a PS5 back into Linux after the PS5 Linux loader sends
its UDP beacon and the console enters rest mode.

VIDEO:
[![PS5 ESP32 Wake setup video](https://img.youtube.com/vi/I5VjPi-z6jg/maxresdefault.jpg)](https://youtu.be/I5VjPi-z6jg)

T-Display ESP32 hardware example: https://www.aliexpress.com/item/1005006860890245.html

This is an unofficial hobby project and is not affiliated with Sony Interactive
Entertainment.

## What You Need

- LilyGO/TTGO T-Display ESP32, or ESP32-DevKitC without display.
- A PS3, PS4, or PS5 controller paired to the PlayStation.
- USB cable for the controller.
- USB cable for the ESP32.
- ESP-IDF installed.
- Modified Linux loader from https://github.com/bizkut/ps5-umtx2-host

## Install ESP-IDF

```sh
mkdir -p "$HOME/.espressif/v6.0.1"
cd "$HOME/.espressif/v6.0.1"
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32
. ./export.sh
idf.py --version
```

## Configure And Flash

Clone this repository, then install the Python helper dependency:

```sh
python3 -m pip install -r requirements.txt
```

Plug the paired controller into your computer with USB, then read the controller
and paired PlayStation Bluetooth addresses:

```sh
./tools/read_controller_addresses.py
```

Unplug the controller, plug in the ESP32, then run the setup wizard:

```sh
./tools/setup_wizard.py
```

The wizard asks for:

- board profile
- Wi-Fi SSID and password
- Linux loader token, default `ps5-linux`
- ESP32 serial port
- action: configure, build, flash, or monitor

For non-interactive flashing on macOS, use a command like:

```sh
./tools/setup_wizard.py --non-interactive --board tdisplay --action flash --port /dev/cu.usbserial-XXXXXXXX
```

## Use It

1. Keep the ESP32 powered on.
2. Inject the Linux loader from https://github.com/bizkut/ps5-umtx2-host
3. The ESP32 screen changes to `LINUX TIME`.
4. When the PS5 enters rest mode, the screen changes to `WAIT REST`.
5. When the PS5 network comes back, ESP32 sends the Bluetooth wake signal.
6. The screen shows `WAKING UP`, then `LINUX BOOT`, `LINUX ONLINE`,
   `LINUX OFFLINE`, and finally returns to `READY`.

GPIO0 on the T-Display can also manually send the Bluetooth wake signal any time
after boot.

## More Information

Detailed setup notes, board profiles, loader patch notes, and technical details
are in [docs/technical-details.md](docs/technical-details.md).

The Bluetooth wake approach and USB controller address extraction are based on
the Apache-2.0 licensed upstream project:
https://github.com/FreeTHX/pywakepsXonbt
