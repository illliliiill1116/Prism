#!/bin/sh
# Bakes one region with the native build and the browser's (make wasm, run
# under Node), and checks they bake the same: the texture atlas's pixels,
# every tile part decompressed, and the collision file. The compressed
# bytes may differ (each links its own zlib), nothing else may.
#
#   scripts/wasm_check.sh <client.jar> <world or region dir> <rx> <rz> [threads]
set -e
[ $# -ge 4 ] || { echo "usage: $0 <client.jar> <world or region dir> <rx> <rz> [threads]" >&2; exit 2; }
jar=$1 world=$2 rx=$3 rz=$4 threads=${5:-16}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
export PRISM_SKINS="$out/noskins"

t0=$(date +%s.%N)
build/prism bake "$jar" "$world" "$rx" "$rz" "$out/native" "$threads" > "$out/native.log"
t1=$(date +%s.%N)
node build-wasm/prism.js bake "$jar" "$world" "$rx" "$rz" "$out/wasm" "$threads" > "$out/wasm.log"
t2=$(date +%s.%N)
echo "native $(echo "$t1 - $t0" | bc) s, wasm $(echo "$t2 - $t1" | bc) s"

python3 -I - "$out/native" "$out/wasm" <<'EOF'
import struct, sys, zlib

def pixels(path):
    d = open(path, "rb").read()
    i, idat = 8, b""
    while i < len(d):
        n, = struct.unpack(">I", d[i:i + 4]); t = d[i + 4:i + 8]
        if t == b"IHDR": w, h = struct.unpack(">II", d[i + 8:i + 16])
        if t == b"IDAT": idat += d[i + 8:i + 8 + n]
        i += 12 + n
    raw, st, out, prev = zlib.decompress(idat), w * 4, bytearray(), bytearray(w * 4)
    for y in range(h):
        f, line = raw[y * (st + 1)], bytearray(raw[y * (st + 1) + 1:(y + 1) * (st + 1)])
        for x in range(st):
            a = line[x - 4] if x >= 4 else 0; b = prev[x]; c = prev[x - 4] if x >= 4 else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line; prev = line
    return bytes(out)

def parts(path):
    d, out, i = open(path, "rb").read(), [], 0
    while i < len(d):
        o = zlib.decompressobj(31); out.append(o.decompress(d[i:])); i = len(d) - len(o.unused_data)
    return out

n, w = sys.argv[1], sys.argv[2]
bad = 0
if pixels(n + "/atlas.png") != pixels(w + "/atlas.png"): print("atlas: pixels differ"); bad = 1
a, b = parts(n + "/tiles.bin"), parts(w + "/tiles.bin")
diff = [k for k in range(max(len(a), len(b))) if k >= len(a) or k >= len(b) or a[k] != b[k]]
if diff: print(f"tiles: {len(diff)} of {len(a)} parts differ"); bad = 1
if open(n + "/solid.bin", "rb").read() != open(w + "/solid.bin", "rb").read(): print("solid.bin differs"); bad = 1
print("different" if bad else f"same: atlas, {len(a)} tile parts, solid.bin")
sys.exit(bad)
EOF
