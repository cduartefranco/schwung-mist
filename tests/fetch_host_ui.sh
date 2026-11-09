#!/bin/sh
# Copy the Move's OWN schwung UI code next to the canvas harness, so canvas.js is tested
# against the exact host contract the device runs (v0.2's canvas passed an offline test
# built on a different drawing lib, then crash-looped on hardware).
set -e
D="$(cd "$(dirname "$0")" && pwd)/host_ui"
rm -rf "$D" && mkdir -p "$D"
ssh ableton@move.local 'cd /data/UserData/schwung && COPYFILE_DISABLE=1 tar cf - shared host/version.txt shadow/shadow_ui.js' | tar xf - -C "$D"
echo "host UI $(cat "$D/host/version.txt") -> $D"
