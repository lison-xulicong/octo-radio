#!/usr/bin/env bash
# Captures the simulator's screen (the 240x320 canvas, nothing else) as a video.
#
#   tools/octopus/capture_video.sh [seconds] [out.mp4] [--zoom N] [--hidpi N]
#
# The simulator animates at 18 fps, one `clock++` per frame, so the capture
# drives it frame by frame instead of sampling in real time: the browser's own
# rAF loop is stopped first, then each frame is advanced and read back from the
# canvas. That makes the video exactly as smooth as the device, however slow
# the capture itself is. Playback starts a few seconds in, as if the dial were
# pressed, so the clip shows both the idle and the playing rhythm.
#
#   --hidpi N  render the simulator's canvas N times larger, so the text and
#              panels are rasterized at N times the resolution. The scene
#              stays pixel art. Default 4: the text then reads clearly in the
#              video, at the cost of being smoother than the device's own
#              bitmap fonts, which is what you want for showing the design.
#   --zoom N   enlarge each rendered pixel N more times when encoding (nearest
#              neighbour, so the pixels stay square). Default 2.
#
# For a frame-accurate record of the hardware instead, turn the re-render off
# and enlarge only at encode time: --hidpi 1 --zoom 6.
#
# Needs gstack browse (headless Chromium) and ffmpeg. browse binds a localhost
# port, so run this from a normal terminal, not from a sandboxed tool.
set -euo pipefail

seconds=30
out=""
zoom=2
hidpi=4
args=()
while [ $# -gt 0 ]; do
    case "$1" in
        --zoom) zoom="$2"; shift 2 ;;
        --hidpi) hidpi="$2"; shift 2 ;;
        *) args+=("$1"); shift ;;
    esac
done
[ "${#args[@]}" -ge 1 ] && seconds="${args[0]}"
[ "${#args[@]}" -ge 2 ] && out="${args[1]}"

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ -z "$out" ] && out="$root/build-full/shots/simulator-${seconds}s.mp4"

fps=18
frames=$((seconds * fps))
play_at=$((4 * fps))  # press play after 4 s of idle

B=""
[ -x "$root/.claude/skills/gstack/browse/dist/browse" ] && B="$root/.claude/skills/gstack/browse/dist/browse"
[ -z "$B" ] && B="$HOME/.claude/skills/gstack/browse/dist/browse"
if [ ! -x "$B" ]; then
    echo "gstack browse not found; run its ./setup first" >&2
    exit 1
fi

dir="$root/.capture"
rm -rf "$dir"
mkdir -p "$dir" "$(dirname "$out")"

echo "opening $root/tools/octopus/simulator.html"
"$B" goto "file://$root/tools/octopus/simulator.html" > /dev/null
# Let the fonts load and the first frames settle, then take the loop over: the
# callback already scheduled runs once more and then finds the stub.
sleep 2
"$B" js "window.requestAnimationFrame = function(){ return 0; }; 'loop stopped'" > /dev/null
sleep 0.3

if [ "$hidpi" != "1" ]; then
    # Resizing the canvas keeps the same 2d context, so the scale set here
    # stays in force: every later draw is in device pixels times $hidpi.
    echo "rendering the canvas at ${hidpi}x"
    "$B" js "lcd.width = 240 * $hidpi; lcd.height = 320 * $hidpi; \
             L.imageSmoothingEnabled = false; L.setTransform($hidpi, 0, 0, $hidpi, 0, 0); \
             lcd.width + 'x' + lcd.height" > /dev/null
fi

step="(function(){ clock++; stepMeters(); render(); return document.getElementById('lcd').toDataURL('image/png'); })()"

echo "capturing $frames frames (${seconds}s at ${fps}fps)"
for ((i = 0; i < frames; i++)); do
    if [ "$i" -eq "$play_at" ]; then
        "$B" js "press('ok'); 'playing'" > /dev/null
    fi
    "$B" js "$step" --out "$(printf '%s/f%04d.png' "$dir" "$i")" > /dev/null
    if [ $((i % 60)) -eq 0 ]; then printf '  %d/%d\n' "$i" "$frames"; fi
done

echo "encoding $out"
width=$((240 * hidpi * zoom))
height=$((320 * hidpi * zoom))
# crf 14 keeps the hard pixel edges clean; at this size the chroma that h264
# throws away is well inside one device pixel, so the colours do not bleed.
ffmpeg -y -loglevel error -framerate "$fps" -i "$dir/f%04d.png" \
    -vf "scale=${width}:${height}:flags=neighbor" -c:v libx264 -preset slow -crf 14 \
    -pix_fmt yuv420p -movflags +faststart "$out"
rm -rf "$dir"
ls -la "$out"
