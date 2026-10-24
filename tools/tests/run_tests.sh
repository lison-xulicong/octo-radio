#!/usr/bin/env bash
# Host-side checks for FM-name parsing, dial mapping and the rendering stack.
# These need no device.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The generated font must carry every city/province and UI character. It also
# includes GB2312 so ordinary Chinese station names are not boxed out.
python3 - "$root" <<'PY'
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
source = (root / "main/radio_font.c").read_text(encoding="utf-8")
glyphs = {int(value, 16) for value in re.findall(r"/\* U\+([0-9A-Fa-f]+)", source)}

needed = set()
for suffix in ("*.cc", "*.h"):
    for path in (root / "main").glob(suffix):
        needed.update(ord(ch) for ch in path.read_text(encoding="utf-8") if ord(ch) > 0x7F)

for lead in range(0xB0, 0xF8):
    for trail in range(0xA1, 0xFF):
        try:
            needed.update(ord(ch) for ch in bytes((lead, trail)).decode("gb2312"))
        except UnicodeDecodeError:
            pass

missing = needed - glyphs
if missing:
    raise SystemExit(
        "font is missing glyphs: " + "".join(chr(codepoint) for codepoint in sorted(missing))
    )
print(f"font coverage: ok ({len(needed)} non-ASCII characters)")

title_source = (root / "main/radio_font_title.c").read_text(encoding="utf-8")
title_glyphs = {
    int(value, 16) for value in re.findall(r"/\* U\+([0-9A-Fa-f]+)", title_source)
}
title_missing = {ord(ch) for ch in "联网收音机"} - title_glyphs
if title_missing:
    raise SystemExit("title font is missing required glyphs")
print("title font coverage: ok")
PY

python3 "$root/tools/build_frequency_table.py" \
    --output "$work/radio_frequency_data.h" > /dev/null
if ! cmp -s "$work/radio_frequency_data.h" "$root/main/radio_frequency_data.h"; then
    echo "FAIL: radio_frequency_data.h is stale; rerun tools/build_frequency_table.py" >&2
    exit 1
fi

python3 "$root/tools/build_city_table.py" \
    --output "$work/radio_cities.h" > /dev/null
if ! cmp -s "$work/radio_cities.h" "$root/main/radio_cities.h"; then
    echo "FAIL: radio_cities.h is stale; rerun tools/build_city_table.py" >&2
    exit 1
fi

python3 - "$root/main/radio_catalog.cc" "$work/freq.h" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding='utf-8').read()
start = text.index('// Chinese FM broadcasting occupies')
end = text.index('void make_display_name(')
open(dst, 'w', encoding='utf-8').write(
    '#pragma once\n#include <cstddef>\n#include <cstdint>\nnamespace freq {\n'
    + text[start:end] + '\n}\n')
PY

c++ -std=c++17 -O1 -I "$work" -o "$work/freq" "$here/test_frequency_parsing.cc"
"$work/freq"
c++ -std=c++17 -O1 -o "$work/dial" "$here/test_dial_mapping.cc"
"$work/dial"
c++ -std=c++17 -O1 -I "$root/main" -o "$work/frequency_table" \
    "$here/test_frequency_table.cc" "$root/main/radio_frequency.cc"
"$work/frequency_table"
c++ -std=c++17 -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I "$here/catalog_stubs" -I "$IDF_PATH/components/json/cJSON" \
    -I "$root/main" -o "$work/frequency_remote" \
    "$here/test_frequency_remote.cc" \
    "$root/main/radio_frequency_remote_parse.cc" \
    "$IDF_PATH/components/json/cJSON/cJSON.c"
"$work/frequency_remote"
c++ -std=c++17 -O1 -I "$root/main" -o "$work/mp3_sync" \
    "$here/test_mp3_sync.cc"
"$work/mp3_sync"

# Main-page octopus scene: renders frames against a fake panel and checks
# coverage, the LVGL badge holes, palette use and that it animates.
octopus_cxx=(c++ -std=c++17 -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined
    -DRADIO_OCTOPUS_TESTING=1 -I "$here/octopus_stubs" -I "$here" -I "$root/main")
"${octopus_cxx[@]}" -o "$work/octopus" "$here/test_octopus_render.cc" \
    "$here/octopus_fake_panel.cc" "$root/main/radio_octopus.cc"
"$work/octopus" "$work"

# Firmware frames against the design (the simulator's scene) at fixed moments.
# The design frames come from tools/octopus/reference.html; skip if they have
# not been generated on this machine.
reference="$root/tools/octopus/out/reference_frames.json"
if [ -f "$reference" ]; then
    "${octopus_cxx[@]}" -o "$work/fidelity" "$here/test_octopus_fidelity.cc" \
        "$here/octopus_fake_panel.cc" "$root/main/radio_octopus.cc"
    "$work/fidelity" "$reference" "$work" --gate

    # The lite variant has to stay close to the design as well, and has to
    # differ from full (otherwise the switch does nothing).
    "${octopus_cxx[@]}" -DRADIO_OCTOPUS_LITE=1 -o "$work/fidelity_lite" \
        "$here/test_octopus_fidelity.cc" "$here/octopus_fake_panel.cc" "$root/main/radio_octopus.cc"
    "$work/fidelity_lite" "$reference" "$work" --gate
    "$work/fidelity" "$reference" "$work" | awk '/^(idle|playing) /{print $NF}' > "$work/full.hash"
    "$work/fidelity_lite" "$reference" "$work" | awk '/^(idle|playing) /{print $NF}' > "$work/lite.hash"
    if cmp -s "$work/full.hash" "$work/lite.hash"; then
        echo "FAIL: lite renders exactly like full"
        exit 1
    fi
    echo "lite differs from full: ok"
else
    echo "skipping octopus fidelity: run tools/octopus/reference.html to create $reference"
fi

# Station catalog: pinyin conversion and the streaming parser. The sample
# responses are real directory answers; skipped when they are not around.
samples="${CATALOG_SAMPLES:-}"
if [ -n "$samples" ] && [ -d "$samples" ]; then
    c++ -std=c++17 -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
        -DCATALOG_TESTING=1 -I "$here/catalog_stubs" -I "$IDF_PATH/components/json/cJSON" \
        -I "$root/main" -o "$work/catalog" "$here/test_catalog.cc" \
        "$root/main/radio_catalog.cc" "$root/main/radio_frequency.cc" \
        "$IDF_PATH/components/json/cJSON/cJSON.c"
    "$work/catalog" "$samples"
else
    echo "skipping catalog tests: set CATALOG_SAMPLES to a folder of saved directory responses"
fi
