/* A tile worker: fetches, inflates and decodes tiles off the frame (see
 * loadTile in map.js). */
import { inflate, decodeLevel, decodePack } from "./tile.js";

/* shared parts as fetched, for the tile's other level (jobs go to a
   worker by tile, see loadTile) */
const shared = new Map();
/* Parts of a region's tiles file, [offset, length] each (or null), those
   next to each other in one Range request. A server that does not do
   ranges sends the whole file: then that comes back too, for the page to
   slice the region's other parts from (see loadTile). With the file's
   Last-Modified, for the page to tell a file baked again since its index. */
async function fetchParts(url, parts, sizes) {
    const out = parts.map(() => null), want = parts.map((p, i) => [p, i]).filter(([p]) => p).sort((a, b) => a[0][0] - b[0][0]);
    let whole = null, mod = null;
    for (let k = 0; k < want.length;) {
        const a = want[k][0][0];
        let j = k + 1, end = a + want[k][0][1];
        while (j < want.length && want[j][0][0] === end) end += want[j++][0][1];
        let buf;
        if (whole) buf = whole.slice(a, end);
        else {
            const r = await fetch(url, { headers: { Range: `bytes=${a}-${end - 1}` } });
            if (!r.ok) throw new Error(`${url}: ${r.status}`);
            mod = mod || r.headers.get("Last-Modified");
            buf = await r.arrayBuffer();
            sizes.wire += buf.byteLength;
            if (r.status !== 206) { whole = buf; buf = whole.slice(a, end); }
            else if (buf.byteLength !== end - a) throw new Error(`${url}: bytes ${a}-${end - 1} came as ${buf.byteLength}`);
        }
        for (; k < j; k++) { const [[o, n], i] = want[k]; out[i] = buf.slice(o - a, o - a + n); }
    }
    return { out, whole, mod };
}
/* A job: level 2's pack (pack, parts [it]) or a level (parts [shared,
   own]), fetched from url, or as bufs when the page has the whole file */
onmessage = async e => {
    const { id, url, parts, key, pack, bufs } = e.data;
    try {
        const sizes = { wire: 0, inflated: 0 };
        let got = bufs, whole = null, mod = null;
        if (!got) {
            const s = key && shared.get(key);
            if (s) { shared.delete(key); shared.set(key, s); }
            ({ out: got, whole, mod } = await fetchParts(url, s ? [null, parts[1]] : parts, sizes));
            if (s) got[0] = s;
            else if (key && got[0]) { shared.set(key, got[0]); if (shared.size > 64) shared.delete(shared.keys().next().value); }
        }
        const [a, b] = await Promise.all(got.map(g => g && inflate(g, sizes)));
        const res = pack ? decodePack(a) : decodeLevel(a || null, b || null);
        const xfer = whole ? [whole] : [];
        for (const parts of pack ? res.map(t => t.parts) : [res])
            for (const p of parts) xfer.push(p.pos.buffer, p.uv.buffer, p.col.buffer);
        postMessage({ id, res, sizes, whole, mod }, xfer);
    } catch (err) {
        postMessage({ id, err: String(err) });
    }
};
