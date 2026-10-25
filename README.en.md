# Octo Radio 1.1

English | [简体中文](README.md)

Octo Radio is an ESP32-C3 internet radio with a pixel-art octopus interface.
It provides city discovery and search, station switching, FM metadata,
persistent settings, and three-button operation.

![Octo Radio main screen](assets/screenshots/main.png)

## Features

- IP-based location, built-in cities, and Chinese pinyin/initial search across
  203 cities with currently playable stations.
- Radio Browser station discovery with FM ordering and stream/name deduplication.
- Layered FM resolution: embedded table, Radio Browser tags, then online metadata.
- Persistent city, station, and volume settings with a background catalog refresh.
- MP3 playback, reconnect handling, an 8 KB audio jitter buffer, level meter,
  and volume control.
- On-device Wi-Fi setup and a configuration page at `http://192.168.4.1`.
- Brightness, screen timeout, sleep timer, and saved-network management.

The firmware plays MP3 streams only; it does not include an HLS/AAC player.
Stations requiring authentication, blocked by region, or removed upstream may
still be unavailable. This project does not host station content.

## Hardware

The current BSP targets the following reference board and is not a generic
ESP32-C3 image:

| Part | Configuration |
| --- | --- |
| MCU | ESP32-C3, 160 MHz, 8 MB Flash, no PSRAM |
| Display | ST7789P3-compatible 240×320 TFT over SPI |
| Audio | ES8311 over I2S |
| Battery gauge | CW2017 over I2C |
| Input | Three buttons sharing one ADC input |
| Console | USB Serial/JTAG |

Pins are defined in
[`components/bsp/include/bsp_pins.h`](components/bsp/include/bsp_pins.h).
Porting requires reviewing the display, audio, I2C, and button wiring.

## Quick Start

Requirements: Git, Python 3.9+, and
[ESP-IDF 5.5.3](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/).
The first build needs network access to download components pinned by
`dependencies.lock`.
Set `PORT` to your serial device.

```bash
git clone https://github.com/lison-xulicong/octo-radio.git
cd octo-radio

export IDF_PATH=/path/to/esp-idf
. "$IDF_PATH/export.sh"

./tools/build_variant.sh full
export PORT=/dev/ttyACM0
idf.py -B build-full -p "$PORT" flash monitor
```

On macOS, the serial port is normally under `/dev/cu.usbmodem*`. On Linux it is
normally `/dev/ttyACM0` or `/dev/ttyUSB0`; the user needs serial-device access.

Build variants:

| Variant | Description |
| --- | --- |
| `full` | Complete octopus animation; default |
| `lite` | Reduced arm overdraw |
| `off` | Water background only |

```bash
./tools/build_variant.sh lite
./tools/build_variant.sh off
```

Output is written to `build-<variant>/Octo-Radio.bin`. Build directories, ELF,
map files, `sdkconfig`, and local generated configuration are ignored by Git.

## First Boot

1. Flash the firmware and reset the board.
2. With no saved Wi-Fi, the device starts an `OCTO Radio` access point.
3. Select a 2.4 GHz network on the device, or connect to the access point and
   open `http://192.168.4.1`.
4. After connecting, the saved city and station are loaded and playback begins.

Automatic location uses the public IP address and is city-level. Carrier
routing, CGNAT, and VPNs can make it inaccurate; manual selection and search
remain available. Do not erase NVS unless you intend to remove all settings.

## Controls

| Context | Up / Down | Short OK | Long OK |
| --- | --- | --- | --- |
| Main screen | Previous / next station | Play / pause | Open settings |
| Settings | Select item | Enter or change | Back |
| Keyboard | Move between keys | Type | Alternate action |
| Keyboard, held Up / Down | Scroll one row in the current column | — | — |

Long Up/Down on the main screen adjusts volume.

## City and FM Data

[`data/radio_cities.csv`](data/radio_cities.csv) contains 337 administrative
divisions. [`data/radio_city_availability.csv`](data/radio_city_availability.csv)
records the Radio Browser audit from 2026-10-24:

| Item | Count |
| --- | ---: |
| Administrative source cities | 337 |
| Currently searchable | 203 |
| Excluded: no playable MP3 | 134 |

Only cities with playable MP3 results are embedded at runtime. A missing city
normally means the upstream directory has no matching station, not a keyboard
failure. Refresh the audit and regenerate the firmware table with:

```bash
python3 tools/build_city_table.py --refresh
python3 tools/build_frequency_table.py
IDF_PATH=/path/to/esp-idf ./tools/tests/run_tests.sh
./tools/build_variant.sh full
```

FM data is maintained in [`data/fm_frequencies.csv`](data/fm_frequencies.csv)
and currently contains 87 records across 25 cities. Schema and verification
rules are in [`data/README.md`](data/README.md). The device reads only a compact
generated Flash table, not the CSV. Internet-only stations keep the FM scale
but hide the pointer; the project never invents a terrestrial frequency.

Radio Browser, Qingting metadata, and other upstream services may change or be
temporarily unavailable. The firmware falls back to the embedded table,
station-name parsing, and the last successful catalog.

## Tests

Host tests do not require a connected device:

```bash
IDF_PATH=/path/to/esp-idf ./tools/tests/run_tests.sh
```

They cover the city table, frequency parsing, catalog parsing, MP3
synchronization, dial rendering, fonts, and octopus rendering. To run catalog
checks against saved API responses, provide a fixture directory:

```bash
CATALOG_SAMPLES=/path/to/fixtures \
IDF_PATH=/path/to/esp-idf \
./tools/tests/run_tests.sh
```

## Project Layout

```text
.
├── assets/screenshots/        README screenshots
├── components/bsp/            Display, audio, buttons, battery, and I2C
├── components/esp-wifi-connect Wi-Fi station and web provisioning
├── data/                      City, FM, and audit source data
├── main/                      Firmware, player, catalog, UI, and fonts
├── tools/
│   ├── build_variant.sh       Full/Lite/Off builds
│   ├── build_city_table.py    City availability audit
│   ├── build_frequency_table.py
│   ├── build_radio_font.py
│   ├── octopus/               Animation prototype and asset generation
│   └── tests/                 Host-side tests
├── dependencies.lock          Pinned ESP-IDF component versions
├── partitions.csv
└── sdkconfig.defaults
```

Fonts and octopus assets are committed, so normal builds do not need Node.js.
Node.js is only required when regenerating those assets; see each script's
`--help` output and [`tools/octopus`](tools/octopus).

## Contributing

- Run `IDF_PATH=/path/to/esp-idf ./tools/tests/run_tests.sh` before submitting.
- After changing city data, run `tools/build_city_table.py --refresh` and commit
  the generated `main/radio_cities.h`.
- After changing FM data, run `tools/build_frequency_table.py` and commit the
  generated `main/radio_frequency_data.h`.
- Do not commit `build-*`, `managed_components`, ELF, map, or local `sdkconfig`.
- Stream availability is external; issue reports should include the city,
  station name, and a log with credentials removed.

## Attribution and License

Octo Radio is an independent derivative of
[LEO Radio](https://github.com/leo0183/leo-radio). Both projects use the MIT
license; copyright notices are retained in [`LICENSE`](LICENSE).

Noto Sans SC is licensed under the SIL Open Font License 1.1. License
information for LVGL, Espressif components, `esp-wifi-connect`, and other
dependencies is in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and
[`LICENSES`](LICENSES). Station directory data is provided by the public
[Radio Browser](https://www.radio-browser.info/) API.
