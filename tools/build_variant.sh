#!/usr/bin/env bash
# Builds one octopus variant into its own folder, leaving ./build and ./sdkconfig
# alone, so Full and Lite can sit side by side and be flashed either way.
#
#   tools/build_variant.sh full|lite|off [frame_ms]
#
# Output: build-<variant>/Octo-Radio.bin, plus the exact flash command.
set -euo pipefail

variant="${1:-}"
frame_ms="${2:-}"
case "$variant" in
    full | lite | off) ;;
    *)
        echo "usage: $0 full|lite|off [frame_ms]" >&2
        echo "  full  every layer, as designed (default)" >&2
        echo "  lite  same scene, arms shaded with 3 bands instead of 6" >&2
        echo "  off   water background only, no octopus" >&2
        exit 2
        ;;
esac

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dir="$root/build-$variant"
mkdir -p "$dir"

{
    echo "CONFIG_RADIO_OCTOPUS_QUALITY_FULL=n"
    echo "CONFIG_RADIO_OCTOPUS_QUALITY_LITE=n"
    echo "CONFIG_RADIO_OCTOPUS_QUALITY_OFF=n"
    case "$variant" in
        full) echo "CONFIG_RADIO_OCTOPUS_QUALITY_FULL=y" ;;
        lite) echo "CONFIG_RADIO_OCTOPUS_QUALITY_LITE=y" ;;
        off) echo "CONFIG_RADIO_OCTOPUS_QUALITY_OFF=y" ;;
    esac
    if [ -n "$frame_ms" ]; then echo "CONFIG_RADIO_OCTOPUS_FRAME_MS=$frame_ms"; fi
} > "$dir/variant.defaults"

config_hash="$(
    shasum -a 256 "$root/sdkconfig.defaults" "$dir/variant.defaults" |
        shasum -a 256 |
        awk '{print $1}'
)"
config_stamp="$dir/defaults.sha256"
if [ -f "$dir/sdkconfig" ] &&
    { [ ! -f "$config_stamp" ] || [ "$(cat "$config_stamp")" != "$config_hash" ]; }; then
    # Kconfig keeps choices from an existing generated sdkconfig. Preserve that
    # file as a recoverable backup, then let ESP-IDF regenerate from the current
    # defaults so compiler and LVGL settings actually take effect.
    mv "$dir/sdkconfig" "$dir/sdkconfig.previous"
fi
printf '%s\n' "$config_hash" > "$config_stamp"

if ! command -v idf.py > /dev/null 2>&1; then
    # shellcheck disable=SC1090
    source "${IDF_PATH:-$HOME/esp}/export.sh" > /dev/null
fi

cd "$root"
idf.py -B "$dir" -D "SDKCONFIG=$dir/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;$dir/variant.defaults" build

echo
echo "variant : $variant${frame_ms:+ (frame ${frame_ms} ms)}"
echo "image   : $dir/Octo-Radio.bin ($(wc -c < "$dir/Octo-Radio.bin") bytes)"
echo "flash   : idf.py -B build-$variant -p <PORT> flash"
echo "monitor : idf.py -B build-$variant -p <PORT> monitor   (look for '[$variant] avg frame')"
