/* A baked map as a site of its own, in a zip: the viewer's page
   (viewer/site.html), its scripts and the map, for any static host or to
   pass on. Read over HTTP from the backend, prism serve's or the browser's
   (sw.js serves its maps the same way), so it is one for both. The files
   are stored, not compressed: the tiles are gzipped and the atlases PNGs
   already. */

const enc = new TextEncoder();

/* exportSite({ map, viewer, files, title, start, progress }) -> Blob (a zip)
     map       the map's folder URL, ending in "/"
     viewer    the viewer's folder URL, ending in "/"
     files     the viewer's scripts, paths under viewer
     title     the page's title
     start     the view the page opens at: the viewer's parameters
               ({ cam, at, sun, ... }, strings), or {} for the map's own
     progress  (0..1) => {} */
export async function exportSite({ map, viewer, files, title, start = {}, progress = () => {} }) {
    const get = async (url, optional) => {
        const r = await fetch(url, { cache: "no-store" });
        if (optional && r.status === 404) return null;
        if (!r.ok) throw new Error(`${url}: ${r.status}`);
        return r.blob();
    };
    /* what to fetch: [path in the zip, URL, optional] */
    const want = [["style.css", viewer + "style.css"], ...files.map((f) => [f, viewer + f]), ["map/map.json", map + "map.json"]];
    const mj = JSON.parse(await (await get(map + "map.json")).text());
    for (const r of mj.regions || []) {
        const d = `map/${r.dir}/`;
        want.push([d + "index.json", map + r.dir + "/index.json"], [d + "atlas.png", map + r.dir + "/atlas.png"],
            [d + "tiles.bin", map + r.dir + "/tiles.bin"], [d + "solid.bin", map + r.dir + "/solid.bin", true]);
    }
    const page = (await (await get(viewer + "site.html")).text())
        .replaceAll("@TITLE@", () => html(title))
        .replace("@SUB@", "")
        .replace("@START@", () => startJs(start));
    const zip = new Zip();
    for (const [name, text] of [["index.html", page], ["README.txt", README.replace("@TITLE@", () => title)]]) {
        const b = new Blob([text]);
        zip.add(name, b, await crc(b));
    }
    for (let i = 0; i < want.length; i++) {
        const [path, url, optional] = want[i];
        const b = await get(url, optional);
        if (b) zip.add(path, b, await crc(b));
        progress((i + 1) / want.length);
    }
    return zip.blob();
}

const README = `@TITLE@: a Minecraft world as a 3D map, made with prism.

This folder is a website. Opened as a file (a double click on index.html)
it shows nothing: a browser won't run it from a file, it has to come from
a web server.

To share it, put the whole folder on a static web host (Netlify, GitHub
Pages, Cloudflare Pages or your own server) and open the address it gives
you. The host has to send the files as they are (any static host does).

To look at it on this computer, open the map in the prism app, or serve
this folder, for example with "python -m http.server" in it, and open
http://localhost:8000/ in Chrome or Edge.

The viewer loads three.js from the jsDelivr CDN, so it needs the internet.
Minecraft's textures belong to Mojang; prism is not affiliated with Mojang.
`.replace(/\n/g, "\r\n");

/* the start view as a script's object: names and string values only,
   nothing that could end the script */
function startJs(start) {
    const o = {};
    for (const [k, v] of Object.entries(start || {})) if (/^[a-z]+$/.test(k) && typeof v === "string") o[k] = v;
    return JSON.stringify(o).replace(/</g, "\\u003c");
}

/* The view a map was last left at in the viewer (viewer/index.html keeps
   it), for its export to start at; {} if it was never moved. */
export function lastView(mapUrl) {
    try {
        const key = "prism-view " + decodeURIComponent(new URL(mapUrl, location.href).pathname);
        return JSON.parse(localStorage.getItem(key)) || {};
    } catch { return {}; }
}

export function forgetView(mapUrl) {
    try { localStorage.removeItem("prism-view " + decodeURIComponent(new URL(mapUrl, location.href).pathname)); } catch {}
}

const html = (s) => String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;");

let table;
async function crc(blob) {
    if (!table) {
        table = new Uint32Array(256);
        for (let n = 0; n < 256; n++) {
            let c = n;
            for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
            table[n] = c >>> 0;
        }
    }
    let c = 0xffffffff;
    const reader = blob.stream().getReader();
    for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        for (let i = 0; i < value.length; i++) c = table[(c ^ value[i]) & 255] ^ (c >>> 8);
    }
    return (c ^ 0xffffffff) >>> 0;
}

/* A zip of stored files, put together as a Blob of the files' own Blobs
   (nothing copied). Up to 4 GB, the classic format's limit. */
class Zip {
    constructor() { this.parts = []; this.dir = []; this.at = 0; }
    add(name, blob, crc32) {
        const n = enc.encode(name), size = blob.size;
        if (this.at + size + 30 + n.length > 0xfffffff0) throw new Error("the map is over 4 GB, too big to export in one zip");
        const now = new Date();
        const time = (now.getHours() << 11) | (now.getMinutes() << 5) | (now.getSeconds() >> 1);
        const date = ((now.getFullYear() - 1980) << 9) | ((now.getMonth() + 1) << 5) | now.getDate();
        const head = (sig, extra) => {
            const b = new DataView(new ArrayBuffer(extra));
            b.setUint32(0, sig, true);
            return b;
        };
        const l = head(0x04034b50, 30 + n.length);
        l.setUint16(4, 20, true); l.setUint16(6, 0x800, true); l.setUint16(8, 0, true);
        l.setUint16(10, time, true); l.setUint16(12, date, true);
        l.setUint32(14, crc32, true); l.setUint32(18, size, true); l.setUint32(22, size, true);
        l.setUint16(26, n.length, true); l.setUint16(28, 0, true);
        new Uint8Array(l.buffer).set(n, 30);
        const c = head(0x02014b50, 46 + n.length);
        c.setUint16(4, 20, true); c.setUint16(6, 20, true); c.setUint16(8, 0x800, true); c.setUint16(10, 0, true);
        c.setUint16(12, time, true); c.setUint16(14, date, true);
        c.setUint32(16, crc32, true); c.setUint32(20, size, true); c.setUint32(24, size, true);
        c.setUint16(28, n.length, true);
        c.setUint32(42, this.at, true);
        new Uint8Array(c.buffer).set(n, 46);
        this.parts.push(l.buffer, blob);
        this.dir.push(c.buffer);
        this.at += 30 + n.length + size;
    }
    blob() {
        const len = this.dir.reduce((s, b) => s + b.byteLength, 0);
        const e = new DataView(new ArrayBuffer(22));
        e.setUint32(0, 0x06054b50, true);
        e.setUint16(8, this.dir.length, true); e.setUint16(10, this.dir.length, true);
        e.setUint32(12, len, true); e.setUint32(16, this.at, true);
        return new Blob([...this.parts, ...this.dir, e.buffer], { type: "application/zip" });
    }
}
