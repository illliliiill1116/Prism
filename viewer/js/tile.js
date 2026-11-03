/* prism's tile format, decoded: the counterpart of src/tiles.c. Used by
 * the tile workers (tile-worker.js) and, for its constants, by main.js. */

/* Canopy plants (prism's plant_canopy: grass, kelp grown as foliage,
 * _SUN alpha CANOPY_A) do not fade: level 1 draws them as leaf blocks. */
export const CANOPY_A = 8;
/* The map format this viewer reads (prism's MAP_FORMAT; map.json and each
 * index.json say theirs) and the file a region's tiles are in. */
export const FORMAT = 1, TILES = "tiles.bin";
/* An "M3T9" tile payload (prism's tile_payload says how it is packed) at
 * byte off of buf, unpacked per material into its vertices: nq
 * quads, pos int16 x,y,z and the bounce (packBounce), uv uint16 u,v, col
 * COLOR_0 + _SUN + _SUNWIN + _BLK bytes, CS a vertex. Decoded first at CW
 * bytes a vertex, with _BNC and _BAMP as bytes after those. */
export const CS = 16, CW = 24;
export const LIT_AT = [0, 1, 2, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 16, 17, 18, 20, 21, 22, 23]; /* where a corner's lit bytes go in a CW vertex */
/* prism's bake() constants, which lightFromPar needs (C floats) */
export const LIGHT = Object.fromEntries(Object.entries({
    SKY_FLOOR: 0.20, LIGHT_MIN: 0.25, LIGHT_MAX: 1.40, BLOCK_MAX: 1.9, BLOCK_DAY: 0.5, ALBEDO: 0.88,
    SKY_COL: [0.54, 0.52, 0.61], BLOCK_COL: [1.50, 0.88, 0.38], SUN_COL: [0.78, 0.60, 0.42],
}).map(([k, v]) => [k, Array.isArray(v) ? v.map(Math.fround) : Math.fround(v)]));
/* A corner's COLOR_0, _SUN and _BLK into col at o, from what bake() made
 * them of (prism's light_from_par, step for step): sky rays open (of 16)
 * + 32 x wet samples, block light, tint, the quad's shade, light through
 * water and its colour (wa, wc), no sun when its windows are empty. */
export function lightFromPar(col, o, sky0, blk0, tint, ti, shade, wa, wc, wi, nosun) {
    const L = LIGHT, F = L.SKY_FLOOR;
    const sky = F + (1 - F) * (sky0 & 31) / 16, blk = blk0 / 255, sh = shade / 255 * L.ALBEDO;
    const wet = (sky0 >> 5) / 4, att = wa / 255;
    for (let c = 0; c < 3; c++) {
        const sc = L.SKY_COL[c], bc = L.BLOCK_COL[c], uc = L.SUN_COL[c];
        const base = sc * sky + L.BLOCK_DAY * bc * blk, t = (tint ? tint[ti + c] : 255) / 255 * 0.5;
        let l1 = base < L.LIGHT_MIN ? L.LIGHT_MIN : base > L.LIGHT_MAX ? L.LIGHT_MAX : base;
        let l0 = base + uc > L.LIGHT_MAX ? L.LIGHT_MAX : base + uc;
        let lb = bc * blk > L.BLOCK_MAX ? L.BLOCK_MAX : bc * blk;
        l1 *= sh; l0 *= sh; lb *= sh;
        if (wet > 0) {
            const w = wc[wi + c] / 255, f = 1 - wet + att * (0.75 + 0.25 * w), g = (wet - att) * w * 0.22;
            l1 = l1 * f + g; l0 = l0 * f + g; lb = lb * f + g;
        }
        const v1 = l1 * t, v0 = nosun ? 0 : (l0 - l1) * t, vb = lb * t;
        col[o + c] = v1 >= 1 ? 255 : v1 <= 0 ? 0 : Math.floor(v1 * 255 + 0.5);
        col[o + 4 + c] = v0 >= 1 ? 255 : v0 <= 0 ? 0 : Math.floor(v0 * 255 + 0.5);
        col[o + 12 + c] = vb >= 1 ? 255 : vb <= 0 ? 0 : Math.floor(vb * 255 + 0.5);
    }
}
export const AXES = [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]];
export function decodeTile(buf, off, cont) {
    const dv = new DataView(buf, off);
    if (dv.getUint32(0, true) !== 0x3954334d) throw new Error("not an M3T9 tile");
    const ncol = 21;
    const nq = [], cols = [];
    for (let m = 0; m < 4; m++) nq.push(dv.getUint32(4 + m * 4, true));
    let p = off + 20 + ncol * 4;
    for (let c = 0; c < ncol; c++) {
        const n = dv.getUint32(20 + c * 4, true);
        cols.push([p, n]);
        p += (n + 3) & ~3;
    }
    const u8 = c => new Uint8Array(buf, cols[c][0], cols[c][1]);
    const mode = u8(0), hit = u8(1), dirs = u8(2), lit = u8(7), emits = u8(8), wins = u8(9), blks = u8(10);
    const [shades, skys, bqs, tints, watt, wcol, win2, bamps, bncs, pflags] = [11, 12, 13, 14, 15, 16, 17, 18, 19, 20].map(u8);
    const lens = new Uint16Array(buf, cols[3][0], cols[3][1] >> 1), edge = new Int16Array(buf, cols[4][0], cols[4][1] >> 1);
    const p0 = new Uint16Array(buf, cols[5][0], cols[5][1] >> 1), uvs = new Uint16Array(buf, cols[6][0], cols[6][1] >> 1);
    /* the last colours at a corner of an axis-aligned quad, by position and
       dir: an open-addressing table of keys x,z and y,dir (+1, 0 is empty) */
    const st = cont && cont.state, NL = LIT_AT.length;
    let cap = 16;
    while (cap < (cols[2][1] + (cont && cont.extra || 0)) * 8) cap *= 2;
    if (st) cap = st.cap;
    const kxz = st ? st.kxz : new Int32Array(cap), kyd = st ? st.kyd : new Int32Array(cap), val = st ? st.val : new Uint8Array(cap * NL), mask = cap - 1;
    const P = new Int32Array(12);
    /* windows in 4ths, a second one in win2 when the first's start has 128 */
    const nWin = cols[9][1] >> 1, nWin2 = cols[17][1] >> 1, w4 = c => c === 64 ? 255 : c << 2;
    let iWin2 = 0, iShade = 0, iSky = 0, iBq = 0, iTint = 0, iWa = 0, iBamp = 0, iBnc = 0, iPf = 0;
    let iWin = 0, x = st ? st.x : 0, y = st ? st.y : 0, z = st ? st.z : 0, q = 0, iDir = 0, iLen = 0, iEdge = 0, iUv = 0, iLit = 0, iEmit = 0, iBlk = 0;
    const out = [];
    for (let m = 0; m < 4; m++) {
        const n = nq[m], pos = new Int16Array(n * 16), uv = new Uint16Array(n * 8), col = new Uint8Array(n * 4 * CW);
        for (let i = 0; i < n; i++, q++) {
            const md = mode[q], shape = (md >> 4) & 3;
            x = (x + p0[q * 3]) << 16 >> 16;
            y = (y + p0[q * 3 + 1]) << 16 >> 16;
            z = (z + p0[q * 3 + 2]) << 16 >> 16;
            let dir = -1;
            P[0] = x; P[1] = y; P[2] = z;
            if (shape === 0) {
                dir = dirs[iDir++];
                const a1 = AXES[(dir / 6) | 0], a2 = AXES[dir % 6], l1 = lens[iLen++], l2 = lens[iLen++];
                for (let a = 0; a < 3; a++) {
                    P[3 + a] = P[a] + a1[a] * l1;
                    P[9 + a] = P[a] + a2[a] * l2;
                    P[6 + a] = P[3 + a] + a2[a] * l2;
                }
            } else if (shape === 1) {
                for (let a = 0; a < 3; a++) {
                    P[3 + a] = P[a] + edge[iEdge + a];
                    P[9 + a] = P[a] + edge[iEdge + 3 + a];
                    P[6 + a] = P[3 + a] + edge[iEdge + 3 + a];
                }
                iEdge += 6;
            } else {
                for (let k = 3; k < 12; k++) P[k] = P[k % 3] + edge[iEdge++];
            }
            const v0 = i * 4;
            for (let c = 0; c < 4; c++)
                for (let a = 0; a < 3; a++) pos[(v0 + c) * 4 + a] = P[c * 3 + a];
            if (md & 64) {
                for (let k = 0; k < 8; k++) uv[v0 * 2 + k] = uvs[iUv++];
            } else {
                const k = md & 3, u0 = uvs[iUv], w0 = uvs[iUv + 1], u1 = uvs[iUv + 2], w1 = uvs[iUv + 3];
                iUv += 4;
                for (let c = 0; c < 4; c++) {
                    const j = (c - k) & 3;
                    uv[(v0 + c) * 2] = j === 0 || j === 3 ? u0 : u1;
                    uv[(v0 + c) * 2 + 1] = j < 2 ? w0 : w1;
                }
            }
            const sun = md & 4 /* no sun */, plant = md & 8, h = hit[q], emit = md & 128 ? emits[iEmit++] : 0;
            const sunA = plant ? (h & 16 ? 240 : pflags[iPf++] & 1 ? CANOPY_A : 0) : h & 16 ? 191 : 255;
            /* a quad with hit bit 7 keeps its colours as they are; the rest
               are worked out (lightFromPar) */
            const raw = h & 128, shade = raw ? 0 : shades[iShade++];
            for (let c = 0; c < 4; c++) {
                const o = (v0 + c) * CW;
                let slot = -1;
                if (dir >= 0) {
                    const a = P[c * 3] << 16 | (P[c * 3 + 2] & 0xffff), b = (P[c * 3 + 1] * 64 + dir) + 1;
                    slot = (Math.imul(a, 0x9e3779b1) ^ Math.imul(b, 0x85ebca77)) & mask;
                    while (kyd[slot] && (kxz[slot] !== a || kyd[slot] !== b)) slot = (slot + 1) & mask;
                    kxz[slot] = a; kyd[slot] = b;
                }
                /* COLOR_0 rgb, then _SUN rgb and the windows unless the quad has no
                   sun, then _BLK rgb if the quad has block light */
                if (dir >= 0 && h >> c & 1) {
                    for (let k = 0; k < NL; k++) col[o + LIT_AT[k]] = val[slot * NL + k];
                } else {
                    if (sun) { col[o + 8] = col[o + 10] = 255; col[o + 9] = col[o + 11] = 0; }
                    else {
                        const b0 = wins[iWin];
                        col[o + 8] = w4(b0 & 127); col[o + 9] = w4(wins[nWin + iWin++]);
                        if (b0 & 128) { col[o + 10] = w4(win2[iWin2]); col[o + 11] = w4(win2[nWin2 + iWin2++]); }
                        else { col[o + 10] = 255; col[o + 11] = 0; }
                    }
                    if ((col[o + 20] = bamps[iBamp++])) for (let k = 0; k < 3; k++) col[o + 16 + k] = bncs[iBnc++];
                    if (raw) {
                        const nb = sun ? 3 : 6;
                        for (let k = 0; k < nb; k++) col[o + LIT_AT[k]] = lit[iLit + k];
                        iLit += nb;
                        if (h & 32) { col[o + 12] = blks[iBlk++]; col[o + 13] = blks[iBlk++]; col[o + 14] = blks[iBlk++]; }
                    } else {
                        const s0 = skys[iSky++], nosun = sun || (col[o + 8] > col[o + 9] && col[o + 10] > col[o + 11]);
                        lightFromPar(col, o, s0, h & 32 ? bqs[iBq++] : 0, h & 64 ? tints : null, iTint, shade, s0 >> 5 ? watt[iWa] : 0, wcol, iWa * 3, nosun);
                        if (h & 64) iTint += 3;
                        if (s0 >> 5) iWa++;
                    }
                    if (slot >= 0) for (let k = 0; k < NL; k++) val[slot * NL + k] = col[o + LIT_AT[k]];
                }
                col[o + 3] = emit;
                col[o + 7] = sunA;
                /* for plantSway: hangs from above */
                col[o + 15] = plant && dir < 0 && h & 8 ? 1 : 0;
            }
        }
        out.push({ n, pos, uv, col });
    }
    if (cont) cont.state = { cap, kxz, kyd, val, x, y, z };
    return cont && cont.raw ? out : finishTile(out);
}
/* Decoded quads per material as the meshes want them. */
export function finishTile(raw) {
    return raw.map(({ n, pos, uv, col }, m) => {
        plantSway(n, pos, col);
        const packed = packBounce(n, pos, col);
        return m < 2 ? sortByFacing(n, pos, uv, packed) : { nq: n, pos, uv, col: packed };
    });
}
/* A level of a tile is its shared part (the quads levels 0 and 1 both
 * have) and then its own, which goes on from the state
 * the shared part's decoding left (the colours at corners, the last
 * corner), so the table is sized for both. Either may be missing. */
export function decodeLevel(sBuf, pBuf) {
    const dirs = b => new DataView(b).getUint32(20 + 2 * 4, true);
    const cont = { extra: pBuf ? dirs(pBuf) : 0, raw: true };
    const a = sBuf ? decodeTile(sBuf, 0, cont) : null;
    const b = pBuf ? decodeTile(pBuf, 0, { state: cont.state, raw: true }) : null;
    if (!a || !b) return finishTile(a || b);
    return finishTile(a.map((x, m) => {
        const y = b[m], pos = new Int16Array((x.n + y.n) * 16), uv = new Uint16Array((x.n + y.n) * 8), col = new Uint8Array((x.n + y.n) * 4 * CW);
        pos.set(x.pos); pos.set(y.pos, x.n * 16); uv.set(x.uv); uv.set(y.uv, x.n * 8); col.set(x.col); col.set(y.col, x.n * 4 * CW);
        return { n: x.n + y.n, pos, uv, col };
    }));
}
/* The decoder's CW-byte vertices as CS bytes: _BNC rgb (0..BNC_Q) and
 * _BAMP x (0..BAMP_Q) need 14 bits, not the 8 bytes they took, and go in
 * pos's spare fourth int16 as bamp | r << 5 | g << 8 | b << 11. */
export function packBounce(n, pos, col) {
    const out = new Uint8Array(n * 4 * CS);
    for (let v = 0; v < n * 4; v++) {
        const o = v * CW;
        out.set(col.subarray(o, o + CS), v * CS);
        pos[v * 4 + 3] = col[o + 20] | col[o + 16] << 5 | col[o + 17] << 8 | col[o + 18] << 11;
    }
    return out;
}
/* How far up its plant each plant corner is, for the wind (_BLK alpha:
 * 128 a block, positions in 1/64 block): measured from where the plant
 * stands - a quad's lowest corners, or, when another plant's top is there
 * (the upper half of a tall plant), where that one stands, so the halves
 * bend as one. A flat plant (pink petals' flowers) is as high as it lies
 * above its block's floor, as high as the tops of its stems, so the two
 * move together. Plants that hug a block face (glow lichen, vines: within
 * HUG of a block's side, floor or ceiling) stay still - they would sway
 * into the wall - and those hanging from a block (the decoder marks them
 * in _BLK alpha, from the bake: hit bit 3) are held by their top. */
export function plantSway(n, pos, col) {
    const HUG = 4, B = 64;
    const key = v => ((pos[v * 4] + 32768) * 65536 + (pos[v * 4 + 1] + 32768)) * 65536 + (pos[v * 4 + 2] + 32768);
    const hugs = c => { const m = ((c % B) + B) % B; return m <= HUG || m >= B - HUG; };
    const quads = [], hang = [];
    for (let i = 0; i < n; i++) {
        const hangs = col[i * 4 * CW + 15];
        for (let c = 0; c < 4; c++) col[(i * 4 + c) * CW + 15] = 0;
        if (col[i * 4 * CW + 7] > CANOPY_A) continue; /* plants only (canopy ones too) */
        const lo = [1e9, 1e9, 1e9], hi = [-1e9, -1e9, -1e9];
        for (let c = 0; c < 4; c++)
            for (let a = 0; a < 3; a++) { const v = pos[(i * 4 + c) * 4 + a]; lo[a] = Math.min(lo[a], v); hi[a] = Math.max(hi[a], v); }
        /* lying in a plane along an axis, on a block's face */
        let hug = false;
        for (let a = 0; a < 3; a++) if (lo[a] === hi[a] && hugs(lo[a])) hug = true;
        if (hug) continue;
        if (hangs) hang.push([lo[1], hi[1], i]); else quads.push([lo[1], hi[1], i]);
    }
    /* lower halves first, so an upper half finds where its plant stands */
    quads.sort((a, b) => a[0] - b[0]);
    const ground = new Map();
    for (const [lo, hi, i] of quads) {
        let g = lo === hi ? Math.floor(lo / B) * B : lo;
        if (lo < hi)
            for (let c = 0; c < 4; c++) {
                const v = i * 4 + c;
                if (pos[v * 4 + 1] === lo && ground.has(key(v))) g = ground.get(key(v));
            }
        for (let c = 0; c < 4; c++) {
            const v = i * 4 + c, y = pos[v * 4 + 1];
            if (lo < hi && y === hi) ground.set(key(v), g);
            col[v * CW + 15] = Math.min(255, Math.max(0, Math.round((y - g) * 128 / B)));
        }
    }
    /* hanging ones (marked by the decoder, from the bake) the other way up,
       held where they hang from: top segments first */
    hang.sort((a, b) => b[1] - a[1]);
    const ceil = new Map();
    for (const [lo, hi, i] of hang) {
        let g = hi;
        for (let c = 0; c < 4; c++) {
            const v = i * 4 + c;
            if (pos[v * 4 + 1] === hi && ceil.has(key(v))) g = ceil.get(key(v));
        }
        for (let c = 0; c < 4; c++) {
            const v = i * 4 + c, y = pos[v * 4 + 1];
            if (y === lo) ceil.set(key(v), g);
            col[v * CW + 15] = Math.min(255, Math.max(0, Math.round((g - y) * 128 / B)));
        }
    }
}
/* One-sided quads (OPAQUE, MASK) reordered by where they face, FACE_ORDER:
 * +x, -x, +z, -z, +y, any other way, -y, with each class's count and, for
 * the six axes, the plane nearest the side it faces away from: a +x quad
 * at x = p is seen only from x > p, so from x <= the least p no +x quad is
 * (cls, planes: +x min, -x max, +z min, -z max, +y min, -y max). */
export function sortByFacing(nq, pos, uv, col) {
    const cls = new Uint8Array(nq), count = [0, 0, 0, 0, 0, 0, 0];
    const planes = [Infinity, -Infinity, Infinity, -Infinity, Infinity, -Infinity];
    const SLOT = [0, 1, 4, 6, 2, 3]; /* axis * 2 + negative -> class */
    for (let i = 0; i < nq; i++) {
        const o = i * 16;
        const e1x = pos[o + 4] - pos[o], e1y = pos[o + 5] - pos[o + 1], e1z = pos[o + 6] - pos[o + 2];
        const e2x = pos[o + 12] - pos[o], e2y = pos[o + 13] - pos[o + 1], e2z = pos[o + 14] - pos[o + 2];
        /* the front face's normal for triangles 0, 3, 1 */
        const n = [e2y * e1z - e2z * e1y, e2z * e1x - e2x * e1z, e2x * e1y - e2y * e1x];
        let c = 5;
        for (let a = 0; a < 3; a++)
            if (n[a] && !n[(a + 1) % 3] && !n[(a + 2) % 3]) {
                const neg = n[a] < 0, p = pos[o + a], k = a * 2 + neg;
                c = SLOT[k];
                planes[k] = neg ? Math.max(planes[k], p) : Math.min(planes[k], p);
            }
        cls[i] = c;
        count[c]++;
    }
    const start = [0];
    for (let c = 0; c < 6; c++) start.push(start[c] + count[c]);
    const P = new Int16Array(pos.length), U = new Uint16Array(uv.length), C = new Uint8Array(col.length);
    for (let i = 0; i < nq; i++) {
        const j = start[cls[i]]++;
        P.set(pos.subarray(i * 16, i * 16 + 16), j * 16);
        U.set(uv.subarray(i * 8, i * 8 + 8), j * 8);
        C.set(col.subarray(i * 4 * CS, (i + 1) * 4 * CS), j * 4 * CS);
    }
    return { nq, pos: P, uv: U, col: C, count, planes };
}
/* A tile part (a gzip member) inflated. */
export async function inflate(buf, sizes = {}) {
    const out = await new Response(new Blob([buf]).stream().pipeThrough(new DecompressionStream("gzip"))).arrayBuffer();
    sizes.inflated = (sizes.inflated || 0) + out.byteLength;
    return out;
}
/* A level 2 pack ("M3PK"): its tiles, decoded, with their tx, tz. */
export function decodePack(buf) {
    const dv = new DataView(buf), n = dv.getUint32(4, true), out = [];
    for (let i = 0; i < n; i++) {
        const e = 8 + i * 16;
        out.push({ tx: dv.getInt32(e, true), tz: dv.getInt32(e + 4, true), parts: decodeTile(buf, dv.getUint32(e + 8, true)) });
    }
    return out;
}
