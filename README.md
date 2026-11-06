# ESP32-C3 UART Logger and BLE Bridge

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5-red)
![Target](https://img.shields.io/badge/target-ESP32--C3-blue)
![License](https://img.shields.io/badge/license-MIT-green)

Battery powered serial logger built on the ESP32-C3 with ESP-IDF and NimBLE. It records everything a target device prints on its UART to an SD card. Each target boot gets its own file, named after the target's own clock. When no card is present the logger works as a UART ↔ BLE bridge instead. In both modes the stream is also mirrored to the USB console.

Forked from Henrique Domiciano's ESP32-C3/S3 multi-UART ↔ BLE bridge. That project supplied the BLE GATT server, the MTU-aware notification path and the Python tools in `tools/`.

## Operating modes

The mode is chosen once, at boot:

| SD card at boot | UART0 RX goes to | BLE |
|---|---|---|
| Mounted | SD card and USB console | Non-connectable battery beacon by default (see [Checking a running logger](#checking-a-running-logger)). With `SDCARD_KEEP_BLE` it is connectable, but only BLE → UART0 TX is used. With both options off, NimBLE is not started |
| Absent, mount failed or battery low | BLE notifications and USB console | Advertising, one connection at a time |

The firmware only checks for a card at boot, so a card inserted later is not used until the next reset. If power is cut or the card is pulled while logging, up to the last 30 s of data are lost (see [SD card logging](#sd-card-logging)).

## Hardware

<p align="center">
<img src="docs/esp32c3-mini.png" width="400">
</p>

| Function | GPIO | Set by |
|---|---|---|
| UART0 RX, from target TX | 20 | `app_main()` |
| UART0 TX, to target RX | 21 | `app_main()` |
| SD SPI SCK | 8 | `SDCARD_PIN_CLK` |
| SD SPI MISO | 9 | `SDCARD_PIN_MISO` |
| SD SPI MOSI | 10 | `SDCARD_PIN_MOSI` |
| SD SPI CS | 3 | `SDCARD_PIN_CS` |
| Battery sense, ADC1 channel 2 | 2 | fixed |
| Status LED | 7 | `STATUS_LED_GPIO` |
| USB console | 18 / 19 (USB D- / D+) | built-in USB Serial/JTAG |

- **UART0:** 115200 8N1, no flow control, internal pull-up on RX. The logger must share ground with the target. GPIO20/21 are also the ROM bootloader UART, so the target's RX receives the C3 ROM banner whenever the logger resets.
- **SD card:** must already be FAT formatted, because the firmware never formats it. SPI runs at 20 MHz by default; lower `SDCARD_SPI_FREQ_KHZ` for long or breadboarded wiring.
- **Battery:** connect it to GPIO2 through a resistor divider, by default 1:2 (two equal resistors), which keeps a 1S Li-ion inside the ADC range. The ADC uses 12 dB attenuation, with eFuse curve fitting calibration when the chip has it.
- **Status LED:** gives a 200 ms flash at every boot. During a low battery sleep that means one flash per wake-up period.

## Data path

```text
target TX ──► UART0 RX (16 KB ring) ──► UART task, 2 KB aggregate, sent after 5 ms idle
                                          ├──► USB console                    always, raw bytes
                                          ├──► SD writer (16 KB stream) ──►   FAT file      card mounted
                                          └──► BLE notify 0xABE2              no card, client subscribed

BLE write 0xABE1 ──► UART0 TX ──► target RX
```

## Checking a running logger

The state and battery can be read without stopping the logger or removing the card, from any BLE scanner app such as nRF Connect:

| State | BLE scan |
|---|---|
| Logging to SD | `UART-Logger 3.91V` |
| Logging stopped on a card error | `UART-SD-Fail 3.91V` |
| No card at boot, bridge mode | `UART-to-BLE-Bridge` |
| Low battery sleep, no power or out of range | Nothing |

The name carries the last battery reading and is refreshed every 60 s, so a card error can also take up to 60 s to show. While logging, the advertising is `ADV_NONCONN_IND`, so there is no scan response, no connection and no way to write to the target. Set `SDCARD_BLE_BEACON=n` to keep NimBLE off while logging.

When the scan shows nothing, the status LED tells a low battery sleep, with its one flash per wake-up period, from a logger without power.

## SD card logging

- **One file per target boot.** A line containing `SDCARD_BOOT_MARKER` (default `SoC Firmware version: `) closes the current file and starts a new one. Bytes on that line before the marker go to the previous file.
- **Timestamps:** the target starts its lines with `TAG HH:MM:SS ` (three upper case letters or digits). It prints the date as a `---- YYYY-MM-DD ----` line at boot, at midnight and when its RTC is set. A line's time is dated with the last date line.
- **File names:** `DD-MM_HH-MM-SS.TXT`, taken from the first time in the file. If the name is taken, a `_N` suffix is added.
- **Target RTC not set yet:** the target clock starts at 01-01 00:00:00 after power up. A first time dated 01-01 within `SDCARD_RTC_UNSET_WINDOW_MIN` minutes of midnight is treated as an unset clock. The file is then created as `LOGnnn.TXT`. At the first `RTC SET: YYYY/MM/DD HH:MM:SS` line it is renamed to the real start time: the set time minus the target clock time elapsed since the file's first time.
- **Logger started mid-boot:** when the logger boots while the target is already running, for example after a terminal reset it (see [USB console](#usb-console)), the first line with a time or a date opens a file. Until a date line arrives, the time has no date, so the file is `LOGnnn.TXT`. It is renamed at the first dated time, or at `RTC SET` if that comes first, using the same elapsed time rule.
- **Line noise:** a file is only opened by a printable ASCII line that contains the boot marker, a date or a time. The garbage RX sees while the target is off or powering up therefore creates no files. Dropped bytes are reported once when dropping starts and once, as a total, when the next file opens. Once a file is open, every byte is written as received.
- **File header:** every file starts with the [status line](#status-line).
- **Flushing:** data passes through a 4 KB stdio buffer and is `fflush`ed and `fsync`ed every 30 s.
- **Overflow:** if the writer falls more than 100 ms behind, incoming bytes are dropped and a warning is logged.

The boot marker, the RTC set marker and the date and time layouts all match a specific target firmware. The markers are menuconfig options. The layouts are `SDCARD_TIME_LAYOUT` and `SDCARD_DATE_LAYOUT` in [sdcard.c](main/task/sdcard/sdcard.c).

## BLE bridge

In bridge mode the logger advertises as `UART-to-BLE-Bridge`. Service UUID16 `0xABE0` is in the advertisement in every mode.

| Characteristic | Properties | Data |
|---|---|---|
| `0xABE1` | Write, Write Without Response | client → UART0 TX |
| `0xABE2` | Notify | UART0 RX → client |
| `0xABE4` | Read | Last battery reading in volts as ASCII, e.g. `3.912`, or `n/a` when no reading is available. Up to `BATTERY_PERIOD_MS` old |

- **Connections:** one at a time. No pairing or encryption.
- **Link settings:** the logger asks for an ATT MTU of 517, a 251 byte data length and, when `ENABLE_2M_PHY` is set, the 2M PHY. Notifications are split into chunks of MTU − 3 bytes.
- **On subscribe:** when a client subscribes to `0xABE2`, the first notification it gets is the status line.
- **No subscriber:** UART0 data received while no client is subscribed is not sent over BLE. It still reaches the USB console.

## USB console

The console runs on the C3's built-in USB Serial/JTAG. It shows the UART0 bytes unchanged, and it shows the firmware's own log lines prefixed with `LOG_LINE_PREFIX` (default `[ESP] `). A log line always starts on a new line, even in the middle of a data burst. The default log level is WARN.

The status line is printed each time a terminal opens the port. The C3 USB Serial/JTAG does not expose DTR/RTS, so the firmware infers an open port from the host polling the CDC data endpoint.

Opening the port can reset the logger. The C3 USB Serial/JTAG resets the chip when the host drives RTS, the same way esptool does. `idf.py monitor` does this on purpose (use `--no-reset`), and terminals that apply initial line states on connect can do it too. In CoolTerm, set DTR and RTS to off. Unlike the C6 or H2, the C3 has no register bit to disable this reset. The reset reason is logged at boot, and a USB reset shows as `reset reason: USB`.

## Status line

```text
[ESP] battery 3.912 V, firmware 1.0.1
```

The logger emits this line in three places:

- to a BLE client when it subscribes,
- at the top of every SD file,
- on the USB console whenever a terminal opens the port.

It shows `battery n/a` when no voltage reading is available. The version string lives in [main/version.h](main/version.h).

## Battery and power

- **Sampling:** the battery is read every `BATTERY_PERIOD_MS` (60 s), as the average of `BATTERY_SAMPLES` (16) ADC readings.
- **Low battery:** below `BATTERY_LOW_MV` (3000 mV) the SD file is flushed and closed and the card is unmounted. The firmware waits up to 10 s for that, then the chip enters deep sleep with the LED pad held off.
- **Waking up:** the chip wakes every `BATTERY_WAKE_PERIOD_S` (30 s; 0 means wake only on reset). It resumes only once the pack reads above `BATTERY_RESUME_MV` (3400 mV), so the voltage rebound of an unloaded cell does not restart logging. A reset or power cycle uses `BATTERY_LOW_MV` instead.
- **Low at boot:** if the pack is already low at boot, the chip goes straight to deep sleep without mounting the card.

Power settings:

- **CPU clock:** dynamic frequency scaling between 80 MHz and 40 MHz (XTAL) when idle. Light sleep stays off because it would drop UART0 bytes received while asleep.
- **UART0 clock:** taken from XTAL, so the baud rate does not follow frequency scaling.
- **BT controller:** modem sleep mode 1, with the main XTAL as the low power clock.
- **Advertising interval:** 1000–1100 ms.
- **Connection parameters:** 30–50 ms interval, latency 4, 4 s supervision timeout.
- **TX power:** 0 dBm for advertising and connections.

Measured average current while logging to SD with BLE off is about 12 mA.

## Build

Requires ESP-IDF 5.5 (developed on v5.5.5). The project builds only for the ESP32-C3; CMake rejects other targets.

```bash
idf.py set-target esp32c3
idf.py menuconfig        # optional
idf.py build
idf.py -p <port> flash monitor
```

`sdkconfig` is not versioned. `set-target` generates it from `sdkconfig.defaults` and `sdkconfig.defaults.esp32c3`. An existing `sdkconfig` keeps its values when Kconfig defaults change, so delete it to pick up new defaults.

## Configuration

Project options live in `idf.py menuconfig` under `ESP-UART-BLE-Brigde-Options`:

| Option | Default | |
|---|---|---|
| `ENABLE_2M_PHY` | y | Request the 2M PHY on connect |
| `BLE_TX_POWER_ADV` | 0 | Advertising TX power, dBm, −24 to +18 in 3 dB steps |
| `BLE_TX_POWER_DEFAULT` | 0 | Connection TX power, dBm, same range |
| `STATUS_LED_GPIO` | 7 | Status LED pin |
| `LOG_LINE_PREFIX` | `[ESP] ` | Prefix of firmware log lines and of the status line |
| `ENABLE_SDCARD` | y | Try to mount an SD card at boot |
| `SDCARD_KEEP_BLE` | n | Keep BLE connectable while logging to SD |
| `SDCARD_BLE_BEACON` | y | Advertise the battery voltage while logging to SD |
| `SDCARD_BOOT_MARKER` | `SoC Firmware version: ` | Text that starts a new file |
| `SDCARD_RTC_SET_MARKER` | `RTC SET: ` | Text before the RTC set time |
| `SDCARD_RTC_UNSET_WINDOW_MIN` | 30 | Minutes after 01-01 00:00 treated as an unset clock |
| `SDCARD_PIN_*` | 8 / 9 / 10 / 3 | SCK / MISO / MOSI / CS |
| `SDCARD_SPI_FREQ_KHZ` | 20000 | SPI clock |
| `ENABLE_BATTERY_MONITOR` | y | Battery monitor and low battery sleep |
| `BATTERY_PERIOD_MS` | 60000 | Sampling period |
| `BATTERY_SAMPLES` | 16 | Samples per average |
| `BATTERY_LOW_MV` | 3000 | Low battery threshold |
| `BATTERY_RESUME_MV` | 3400 | Resume threshold after a low battery sleep |
| `BATTERY_WAKE_PERIOD_S` | 30 | Wake-up period during low battery sleep |
| `BATTERY_DIVIDER_RATIO` | 2 | External divider ratio |

These are compile-time constants rather than menuconfig options:

- [main/include.h](main/include.h): BLE name, UUIDs, MTU, advertising and connection timing.
- [main/version.h](main/version.h): firmware version.

## Python tools

`tools/` contains the upstream BLE ↔ TCP bridge, based on bleak, and has not been updated for this firmware. As shipped, neither script works with it:

- **`main.py`:** also subscribes to a second UART channel (`0xABF1` / `0xABF2`) that this firmware does not expose. Removing the `uart1` entry from `channels` in [tools/main.py](tools/main.py) should leave a working UART0 ↔ TCP bridge.
- **`write_baud_rate.py`:** writes to baud rate characteristics (`0xABE3`, `0xABF3`) that this firmware does not expose either.

Clients can only connect when no SD card is mounted, or when `SDCARD_KEEP_BLE` is set. See [tools/README.md](tools/README.md) for usage.

## Repository layout

```text
main/
├── main.c              UART0 task, BLE GATT server, app_main
├── include.h           BLE UUIDs, name and timing
├── version.h           firmware version
├── Kconfig             project options
└── task/
    ├── battery/        ADC monitor, low battery deep sleep
    ├── console/        prefixed USB console, status line
    ├── sdcard/         SD writer, per-boot files and naming
    ├── led/            status LED
    └── ble/            connection count helper
tools/                  upstream Python BLE ↔ TCP bridge
docs/                   board pinout
```

## License

MIT, see [LICENSE](LICENSE).
