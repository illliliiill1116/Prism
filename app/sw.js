/* The browser build's service worker, two jobs:
   - the site's own pages and files get the headers that make the page
     cross-origin isolated (COOP and COEP), which prism's threads need
     (SharedArrayBuffer) and which a static host such as GitHub Pages
     can't be told to send;
   - maps/<map>/... is read from the browser's private storage (OPFS,
     where the page's bakes go), Range requests included, the way the
     viewer reads a map from any server. */
const MAPS = new URL("maps/", self.registration.scope).pathname;

self.addEventListener("install", () => self.skipWaiting());
self.addEventListener("activate", (e) => e.waitUntil(self.clients.claim()));

self.addEventListener("fetch", (e) => {
    const req = e.request, url = new URL(req.url);
    if (url.origin !== location.origin || req.method !== "GET") return;
    if (url.pathname.startsWith(MAPS)) e.respondWith(fromStorage(req, decodeURIComponent(url.pathname.slice(MAPS.length))));
    else e.respondWith(fetch(req).then(isolated, () => new Response("offline", { status: 503 })));
});

function isolated(r) {
    if (r.status === 0 || r.type === "opaque") return r;
    const h = new Headers(r.headers);
    h.set("Cross-Origin-Opener-Policy", "same-origin");
    h.set("Cross-Origin-Embedder-Policy", "credentialless");
    return new Response(r.body, { status: r.status, statusText: r.statusText, headers: h });
}

const TYPES = { json: "application/json", png: "image/png", log: "text/plain; charset=utf-8" };

async function fromStorage(req, rel) {
    const parts = rel.split("/");
    if (!parts.length || parts.some((p) => !p || p === "." || p === "..")) return new Response("bad path", { status: 400 });
    let file;
    try {
        let dir = await (await navigator.storage.getDirectory()).getDirectoryHandle("maps");
        for (const p of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(p);
        file = await (await dir.getFileHandle(parts[parts.length - 1])).getFile();
    } catch {
        return new Response("not found", { status: 404 });
    }
    const h = new Headers({
        "Content-Type": TYPES[rel.split(".").pop()] || "application/octet-stream",
        "Last-Modified": new Date(file.lastModified).toUTCString(),
        "Accept-Ranges": "bytes", "Cache-Control": "no-cache",
        "Cross-Origin-Resource-Policy": "same-origin",
    });
    const m = /^bytes=(\d*)-(\d*)$/.exec(req.headers.get("Range") || "");
    if (!m) {
        h.set("Content-Length", String(file.size));
        return new Response(file, { status: 200, headers: h });
    }
    let a, b;
    if (m[1] === "") { a = Math.max(0, file.size - Number(m[2])); b = file.size - 1; }
    else { a = Number(m[1]); b = m[2] === "" ? file.size - 1 : Math.min(Number(m[2]), file.size - 1); }
    if (a >= file.size || a > b) {
        h.set("Content-Range", `bytes */${file.size}`);
        return new Response("", { status: 416, headers: h });
    }
    h.set("Content-Range", `bytes ${a}-${b}/${file.size}`);
    h.set("Content-Length", String(b - a + 1));
    return new Response(file.slice(a, b + 1), { status: 206, headers: h });
}
