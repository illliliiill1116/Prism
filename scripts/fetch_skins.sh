#!/bin/sh
# fetch_skins.sh <region dir> <skin dir>
#
# Downloads the skin of every player head in <region dir> that <skin dir>
# doesn't hold yet, from textures.minecraft.net, as <skin dir>/<hash>.png
# (prism reads them with PRISM_SKINS=<skin dir>). Skins are content
# addressed, so one never needs fetching twice.
set -e
[ $# -eq 2 ] || { echo "usage: $0 <region dir> <skin dir>" >&2; exit 2; }
bin=$(dirname "$0")/../build/skinlist
mkdir -p "$2"
got=0 have=0 failed=0
for h in $("$bin" "$1"); do
    if [ -s "$2/$h.png" ]; then have=$((have + 1)); continue; fi
    if curl -sSf --max-time 20 -o "$2/$h.png.part" "http://textures.minecraft.net/texture/$h"; then
        mv "$2/$h.png.part" "$2/$h.png"; got=$((got + 1))
    else
        rm -f "$2/$h.png.part"; failed=$((failed + 1))
    fi
done
echo "skins: $got fetched, $have already here, $failed failed"
