# Third-Party Notices

Octo Radio includes or builds upon the following third-party works. The
versions used by this release are recorded in `dependencies.lock`.

## esp-wifi-connect

- Project: <https://github.com/78/esp-wifi-connect>
- Copyright: Copyright (c) 2024 Terrence
- License: MIT
- Local license: [`components/esp-wifi-connect/LICENSE`](components/esp-wifi-connect/LICENSE)

## Noto Sans SC

- Project: <https://github.com/google/fonts/tree/main/ofl/notosanssc>
- Copyright: Copyright 2014-2021 Adobe, with Reserved Font Name "Source"
- License: SIL Open Font License 1.1
- Local license: [`LICENSES/OFL-1.1.txt`](LICENSES/OFL-1.1.txt)

The generated bitmap fonts in `main/radio_font.c` and
`main/radio_font_title.c` are derived from this Font Software.

## LVGL

- Project: <https://github.com/lvgl/lvgl>
- Version: LVGL 9.5.0
- Copyright: LVGL Kft
- License: MIT

The exact upstream license text is included in the LVGL component downloaded by
the ESP-IDF Component Manager.

## Espressif Components

- `espressif/button` 4.2.0: Apache License 2.0
- `espressif/esp_audio_codec` 2.5.0: Espressif Modified MIT License
- `espressif/esp_codec_dev` 1.6.2: Apache License 2.0
- `espressif/esp_lvgl_port` 2.9.0: Apache License 2.0
- `espressif/cmake_utilities` 1.1.1: see the component package
- ESP-IDF 5.5.3 components: see the ESP-IDF distribution

The Component Manager downloads these packages and their license texts into
`managed_components` and the ESP-IDF installation.

## City Metadata and Pinyin

- Administrative divisions: [Administrative-divisions-of-China](https://github.com/modood/Administrative-divisions-of-China), WTFPL
- Pinyin data generation: [python-pinyin](https://github.com/mozillazg/python-pinyin), MIT

The generated city table is checked in as `main/radio_cities.h`.

## Radio Browser

Station metadata and stream URLs are queried from the public
[Radio Browser](https://www.radio-browser.info/) service. Octo Radio does not
claim ownership of station names, audio streams, or other content provided by
third parties.

## Offline Frequency Data

`data/fm_frequencies.csv` contains manually reviewed community and
station-declared frequency entries. Each row records its confidence, source
description, and review date. Entries are not copied from a proprietary
transmitter database.
