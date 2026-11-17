#!/bin/sh
# The demo site: a piece of a world baked on its own (prism map --clip) and
# put in viewer/site.html with the viewer, as a static site.
#
#   scripts/demo.sh <client.jar> <world dir> <x0,z0,x1,z1,ymin>
#                   [--out dir] [--title text] [--cam x,y,z --at x,y,z] [--sun deg]
#                   [--view 'cam=...&at=...&sun=...']
#
# The box is world blocks, both corners inside, and nothing under ymin.
# --out defaults to build-demo/, --title to the world's name, --sun to 100.
# Without --cam and --at the page starts looking at the box from a little
# past its north-west corner, above it. --view is the whole start view
# instead, as the viewer's address keeps it (its parameters after the ?,
# copied from the address bar: the lens and the time of day too).
set -eu
usage="usage: $0 <client.jar> <world dir> <x0,z0,x1,z1,ymin> [--out dir] [--title text] [--cam x,y,z --at x,y,z] [--sun deg] [--view params]"
[ $# -ge 3 ] || { echo "$usage" >&2; exit 2; }
jar=$1 world=$2 box=$3
shift 3
out=build-demo title= cam= at= sun=100 view=
while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || { echo "$usage" >&2; exit 2; }
    case $1 in
    --out) out=$2 ;;
    --title) title=$2 ;;
    --cam) cam=$2 ;;
    --at) at=$2 ;;
    --sun) sun=$2 ;;
    --view) view=$2 ;;
    *) echo "$usage" >&2; exit 2 ;;
    esac
    shift 2
done
if { [ -n "$cam" ] && [ -z "$at" ]; } || { [ -z "$cam" ] && [ -n "$at" ]; }; then echo "--cam and --at go together" >&2; exit 2; fi
here=$(cd "$(dirname "$0")/.." && pwd)
prism=$here/build/prism
[ -x "$prism" ] || { echo "build prism first (make)" >&2; exit 1; }

IFS=, read -r x0 z0 x1 z1 ymin <<EOF
$box
EOF
[ "$x0" -gt "$x1" ] && { t=$x0; x0=$x1; x1=$t; }
[ "$z0" -gt "$z1" ] && { t=$z0; z0=$z1; z1=$t; }
[ -n "$title" ] || title=$("$prism" info "$world" | sed -n 's/.*"name":"\([^"]*\)".*/\1/p')

if [ -z "$cam" ]; then
    # a little past the north-west corner, as high as the box is wide
    s=$(( x1 - x0 > z1 - z0 ? x1 - x0 + 1 : z1 - z0 + 1 ))
    ay=$(( ymin + 25 ))
    cam="$(( x0 - s * 85 / 100 )),$(( ay + s * 12 / 10 )),$(( z0 - s * 85 / 100 ))"
    at="$(( (x0 + x1) / 2 )),$ay,$(( (z0 + z1) / 2 ))"
fi
num() { printf '%s' "$1" | grep -Eq "^-?[0-9.]+$2\$" || { echo "not a number list: $1" >&2; exit 2; }; }
num "$cam" '(,-?[0-9.]+){2}'; num "$at" '(,-?[0-9.]+){2}'; num "$sun" ''
start="{ cam: [$cam], at: [$at], sun: $sun }"
if [ -n "$view" ]; then
    # name=value&...: names of letters, values numbers (lists), as strings
    start=
    for kv in $(printf '%s' "${view#\?}" | tr '&' ' '); do
        k=${kv%%=*} v=${kv#*=}
        printf '%s' "$k" | grep -Eq '^[a-z]+$' && printf '%s' "$v" | grep -Eq '^-?[0-9.]+(,-?[0-9.]+)*$' ||
            { echo "--view: not a view parameter: $kv" >&2; exit 2; }
        start="$start${start:+, }\"$k\": \"$v\""
    done
    start="{ $start }"
fi
rm -rf "$out"
mkdir -p "$out"
"$prism" map "$jar" "$world" "$out/map" --clip "$x0,$z0,$x1,$z1,$ymin"
rm -f "$out"/map/*.log
cp -r "$here/viewer/js" "$here/viewer/style.css" "$out/"

# the title as HTML, then as sed's replacement text; the page has no
# caption (an export's names its world and area): the demo is just the view
esc() { printf '%s' "$1" | sed 's/&/\&amp;/g; s/</\&lt;/g; s/[&|\\]/\\&/g'; }
sed -e "s|@TITLE@|$(esc "$title")|g" \
    -e "s|@SUB@|x $x0 to $x1, z $z0 to $z1, from y $ymin up · |" \
    -e "s|@START@|$start|" \
    -e '/<div class="cap">/,/<\/div>/d' \
    "$here/viewer/site.html" > "$out/index.html"
echo "demo: $out/ (serve it over HTTP and open index.html)"
