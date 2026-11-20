/* The area picker: a world as prism overview's pictures (each region drawn
   isometric, in prism iso's projection), on which an area is dragged out
   to bake as a map of its own (prism map --clip).

   openPicker({ backend, world, jar, T, estimate, onBake }) puts it over the
   page; onBake([x0, z0, x1, z1, ymin]) when the area is chosen. The
   backend gives regions(world) and overview(world, jar, rx, rz): a
   region's picture's corner and size in projected pixels, its URL, and
   its heights' (hpng: the world y each pixel shows, so the block under the
   pointer is the one it points at, and what the area takes is lit up
   pixel by pixel). */

const REF_Y = 64; /* where there are no heights (nothing drawn): the ground the drag is read on */
const EPS = 1 / 64; /* into what a pixel shows, so a side's pixels are its block's */
const REG = 512;

/* prism iso's projection (isodraw.c's iso_project), s pixels a block */
const project = (s, x, y, z) => [(x - z) * s, ((x + z) * s) / 2 + (320 - y) * s + s / 2];
function unproject(s, sx, sy, y) {
    const a = sx / s, b = ((sy - (320 - y) * s - s / 2) * 2) / s;
    return [(a + b) / 2, (b - a) / 2];
}

/* prism overview's heights picture as one number a pixel: (y + 64) * 128
   + 1 in red (high) and green, 0 where nothing is. Read as stored: no
   colour management, no premultiplying. */
async function heights(url, w, h) {
    const bmp = await createImageBitmap(await (await fetch(url)).blob(), { colorSpaceConversion: "none", premultiplyAlpha: "none" });
    const c = document.createElement("canvas");
    c.width = w; c.height = h;
    const g = c.getContext("2d", { willReadFrequently: true });
    g.drawImage(bmp, 0, 0);
    bmp.close();
    const d = g.getImageData(0, 0, w, h).data, out = new Uint16Array(w * h);
    for (let i = 0; i < out.length; i++) if (d[i * 4 + 3] === 255) out[i] = (d[i * 4] << 8) | d[i * 4 + 1];
    return out;
}

export function openPicker({ backend, world, jar, T, estimate, onBake }) {
    const S = 2; /* prism overview's scale */
    const el = (tag, props = {}, ...kids) => {
        const e = Object.assign(document.createElement(tag), props);
        for (const k of kids) if (k != null) e.append(k);
        return e;
    };
    const canvas = el("canvas");
    const status = el("span", { className: "meta" });
    const what = el("div", { className: "pk-what" }, T.pickHint);
    const ymin = el("input", { type: "range", min: -64, max: 200, value: 40 });
    let cutMoved = false;
    const yval = el("output");
    const go = el("button", { className: "go", disabled: true }, T.start);
    const close = el("button", {}, T.cancel);
    const box = el("div", { className: "picker" },
        el("div", { className: "pk-top" }, el("b", {}, `${T.pickTitle} ${world.name}`), status, close),
        canvas,
        el("div", { className: "pk-bar" }, what,
            el("label", { title: T.cutHint }, T.cut, ymin, yval), go));
    document.body.append(box);
    document.body.classList.add("picking");
    const ctx = canvas.getContext("2d");

    /* the view: projected pixel (cx, cy) at the centre, zoom screen pixels to one */
    const sp = project(S, world.spawn[0], REF_Y, world.spawn[1]);
    let cx = sp[0], cy = sp[1], zoom = 0.5, W = 1, H = 1, dpr = 1;
    let regions = [], sel = null, dead = false, refY = REF_Y;
    const urls = [];

    const toScreen = (px, py) => [(px - cx) * zoom + W / 2, (py - cy) * zoom + H / 2];
    /* the world y projected pixel px, py shows (the front region's), or null */
    function heightAt(px, py) {
        for (let k = regions.length - 1; k >= 0; k--) {
            const r = regions[k];
            if (!r.hts) continue;
            const ix = Math.floor(px - r.x), iy = Math.floor(py - r.y);
            if (ix < 0 || iy < 0 || ix >= r.w || iy >= r.h) continue;
            const v = r.hts[iy * r.w + ix];
            if (v) return (v - 1) / 128 - 64;
        }
        return null;
    }
    /* the block the pointer is on: what that pixel shows, or else on REF_Y */
    function toWorld(mx, my) {
        const px = cx + (mx - W / 2) / zoom, py = cy + (my - H / 2) / zoom;
        const y = heightAt(px, py);
        if (y === null) return unproject(S, px, py, REF_Y).map(Math.floor);
        return unproject(S, Math.floor(px) + 0.5, Math.floor(py) + 0.5, y - EPS).map(Math.floor);
    }

    function resize() {
        dpr = window.devicePixelRatio || 1;
        W = canvas.clientWidth;
        H = canvas.clientHeight;
        canvas.width = Math.round(W * dpr);
        canvas.height = Math.round(H * dpr);
        draw();
    }

    let frame = 0;
    function draw() {
        if (frame || dead) return;
        frame = requestAnimationFrame(() => {
            frame = 0;
            ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
            ctx.clearRect(0, 0, W, H);
            ctx.imageSmoothingQuality = "high";
            /* back to front, as prism iso draws */
            for (const r of regions) {
                if (r.img && r.img.complete && r.w) {
                    const [x, y] = toScreen(r.x, r.y);
                    ctx.drawImage(r.img, x, y, r.w * zoom, r.h * zoom);
                } else if (!r.img) outline(r.rx * REG, r.rz * REG, r.rx * REG + REG, r.rz * REG + REG, REF_Y, "rgba(128,128,128,.35)", []);
            }
            if (sel) {
                const [x0, z0, x1, z1] = area();
                const y = +ymin.value;
                /* what the area takes, as it is: the rest dimmed */
                highlight();
                ctx.fillStyle = "rgba(10,12,16,.55)";
                ctx.fillRect(0, 0, W, H);
                for (const r of regions)
                    if (r.hl) {
                        const [x, y] = toScreen(r.x + r.hl.bx, r.y + r.hl.by);
                        ctx.drawImage(r.hl.c, x, y, r.hl.c.width * zoom, r.hl.c.height * zoom);
                    }
                outline(x0, z0, x1 + 1, z1 + 1, y, "rgba(255,255,255,.9)", [4, 4]);
                outline(x0, z0, x1 + 1, z1 + 1, refY, "#e08a5a", []);
                ctx.beginPath();
                for (const [x, z] of [[x0, z1 + 1], [x1 + 1, z1 + 1], [x1 + 1, z0]]) {
                    ctx.moveTo(...toScreen(...project(S, x, refY, z)));
                    ctx.lineTo(...toScreen(...project(S, x, y, z)));
                }
                ctx.strokeStyle = "rgba(255,255,255,.9)";
                ctx.setLineDash([4, 4]);
                ctx.stroke();
                ctx.setLineDash([]);
                /* the handles: corners and the middles of the sides */
                for (const h of handles()) {
                    ctx.beginPath();
                    ctx.arc(h.p[0], h.p[1], h.fx != null && h.fz != null ? 6 : 5, 0, 2 * Math.PI);
                    ctx.fillStyle = "#fff";
                    ctx.fill();
                    ctx.lineWidth = 2;
                    ctx.strokeStyle = "#e08a5a";
                    ctx.stroke();
                }
            }
        });
    }

    /* Each region's pixels that show a block in the area, at or above
       ymin, as a canvas of its picture's (r.hl), made again when the area
       changes; and refY, the mean height they show, for the outline */
    let hlKey = "";
    function highlight() {
        const [x0, z0, x1, z1] = area(), y0 = +ymin.value, key = [x0, z0, x1, z1, y0, regions.filter((r) => r.hts).length].join();
        if (key === hlKey) return;
        hlKey = key;
        let sum = 0, n = 0;
        /* the area's box on the picture plane, ymin to the top */
        const pts = [];
        for (const [x, z] of [[x0, z0], [x1 + 1, z0], [x0, z1 + 1], [x1 + 1, z1 + 1]]) pts.push(project(S, x, y0, z), project(S, x, 320, z));
        const lo = [Math.min(...pts.map((p) => p[0])), Math.min(...pts.map((p) => p[1]))];
        const hi = [Math.max(...pts.map((p) => p[0])), Math.max(...pts.map((p) => p[1]))];
        for (const r of regions) {
            r.hl = null;
            if (!r.hts || !r.img) continue;
            const bx = Math.max(0, Math.floor(lo[0] - r.x)), by = Math.max(0, Math.floor(lo[1] - r.y));
            const ex = Math.min(r.w, Math.ceil(hi[0] - r.x)), ey = Math.min(r.h, Math.ceil(hi[1] - r.y));
            if (bx >= ex || by >= ey) continue;
            const bw = ex - bx, bh = ey - by, mask = new ImageData(bw, bh), m = mask.data;
            let any = false;
            for (let iy = 0; iy < bh; iy++) {
                const py = r.y + by + iy + 0.5, row = (by + iy) * r.w + bx;
                for (let ix = 0; ix < bw; ix++) {
                    const v = r.hts[row + ix];
                    if (!v) continue;
                    const y = (v - 1) / 128 - 64 - EPS;
                    if (y < y0) continue;
                    const [x, z] = unproject(S, r.x + bx + ix + 0.5, py, y);
                    if (x < x0 || x >= x1 + 1 || z < z0 || z >= z1 + 1) continue;
                    m[(iy * bw + ix) * 4 + 3] = 255;
                    sum += y; n++;
                    any = true;
                }
            }
            if (!any) continue;
            const c = document.createElement("canvas");
            c.width = bw; c.height = bh;
            const g = c.getContext("2d");
            g.putImageData(mask, 0, 0);
            g.globalCompositeOperation = "source-in";
            g.drawImage(r.img, -bx, -by);
            r.hl = { c, bx, by };
        }
        if (!mode) refY = n ? Math.round(sum / n) : REF_Y; /* not while dragging: the handles would move under the pointer */
    }

    /* the rectangle x0..x1, z0..z1 at height y */
    function outline(x0, z0, x1, z1, y, color, dash) {
        ctx.beginPath();
        [[x0, z0], [x1, z0], [x1, z1], [x0, z1]].forEach(([x, z], i) => {
            const p = toScreen(...project(S, x, y, z));
            i ? ctx.lineTo(...p) : ctx.moveTo(...p);
        });
        ctx.closePath();
        ctx.setLineDash(dash);
        ctx.lineWidth = 2;
        ctx.strokeStyle = color;
        ctx.stroke();
        ctx.setLineDash([]);
    }

    const area = () => [Math.min(sel[0], sel[2]), Math.min(sel[1], sel[3]), Math.max(sel[0], sel[2]), Math.max(sel[1], sel[3])];

    /* where the handles are on the screen; fx, fz: the side each moves
       (0 the low one, 1 the high one, null neither) */
    function handles() {
        const [x0, z0, x1, z1] = area(), xs = [x0, (x0 + x1 + 1) / 2, x1 + 1], zs = [z0, (z0 + z1 + 1) / 2, z1 + 1];
        const out = [];
        for (let i = 0; i < 3; i++)
            for (let j = 0; j < 3; j++)
                if (i !== 1 || j !== 1)
                    out.push({ fx: i === 1 ? null : i / 2, fz: j === 1 ? null : j / 2, p: toScreen(...project(S, xs[i], refY, zs[j])) });
        return out;
    }
    const handleAt = (mx, my) => sel && handles().find((h) => (h.p[0] - mx) ** 2 + (h.p[1] - my) ** 2 < 12 * 12);
    function inside(mx, my) {
        if (!sel) return false;
        const [x, z] = toWorld(mx, my), [x0, z0, x1, z1] = area();
        return x >= x0 && x <= x1 && z >= z0 && z <= z1;
    }

    function showArea() {
        yval.textContent = ymin.value;
        if (!sel) { what.textContent = T.pickHint; go.disabled = true; return; }
        const [x0, z0, x1, z1] = area();
        const n = new Set();
        for (let rx = x0 >> 9; rx <= x1 >> 9; rx++) for (let rz = z0 >> 9; rz <= z1 >> 9; rz++) n.add(rx + "," + rz);
        what.textContent = `x ${x0} to ${x1}, z ${z0} to ${z1} (${x1 - x0 + 1} × ${z1 - z0 + 1}) · ${T.about} ${estimate(n.size)}` +
            (x1 - x0 > 1024 || z1 - z0 > 1024 ? ` · ${T.pickBig}` : "");
        go.disabled = x1 - x0 < 1 || z1 - z0 < 1;
    }

    /* the pictures: those on the screen first, nearest the centre first, one at a time */
    let loading = false;
    async function loadNext() {
        if (loading || dead) return;
        const [mx, my] = [cx, cy];
        const todo = regions.filter((r) => !r.img && !r.failed && onScreen(r));
        status.textContent = todo.length ? `${T.drawing} (${regions.filter((r) => r.img).length} / ${regions.length})` : "";
        if (!todo.length) return;
        const d = (r) => { const [x, y] = project(S, r.rx * REG + REG / 2, REF_Y, r.rz * REG + REG / 2); return (x - mx) ** 2 + (y - my) ** 2; };
        todo.sort((a, b) => d(a) - d(b));
        const r = todo[0];
        loading = true;
        try {
            const o = await backend.overview(world.id, jar, r.rx, r.rz);
            if (o.error) throw new Error(o.error);
            Object.assign(r, { x: o.x, y: o.y, w: o.w, h: o.h });
            /* the cut starts at 40, or 8 under the lowest ground seen (a
               world built lower), until it is moved */
            if (!cutMoved && o.w && o.ground != null) {
                const v = Math.max(-64, Math.min(40, o.ground - 8));
                if (v < +ymin.value) { ymin.value = v; showArea(); }
            }
            const img = new Image();
            if (o.w && o.png) {
                img.src = o.png;
                if (o.png.startsWith("blob:")) urls.push(o.png);
                await img.decode().catch(() => {});
            }
            if (o.w && o.hpng) {
                if (o.hpng.startsWith("blob:")) urls.push(o.hpng);
                r.hts = await heights(o.hpng, o.w, o.h).catch(() => null);
            }
            r.img = img;
        } catch (e) {
            r.failed = true;
        }
        loading = false;
        draw();
        loadNext();
    }
    function onScreen(r) {
        const pts = [[0, 0], [1, 0], [0, 1], [1, 1]].map(([a, b]) => toScreen(...project(S, (r.rx + a) * REG, REF_Y, (r.rz + b) * REG)));
        const xs = pts.map((p) => p[0]), ys = pts.map((p) => p[1]);
        const m = 200;
        return Math.max(...xs) > -m && Math.min(...xs) < W + m && Math.max(...ys) > -m - 400 * zoom && Math.min(...ys) < H + m;
    }

    /* drag: left on a handle moves that side or corner, inside the area
       moves it whole, elsewhere draws a new one; right or middle (or two
       fingers) moves the view; the wheel zooms */
    const pointers = new Map();
    let mode = null, last = null, grab = null;
    canvas.addEventListener("contextmenu", (e) => e.preventDefault());
    canvas.addEventListener("pointerdown", (e) => {
        canvas.setPointerCapture(e.pointerId);
        pointers.set(e.pointerId, [e.offsetX, e.offsetY]);
        const left = e.pointerType === "touch" ? pointers.size === 1 : e.button === 0 && !e.shiftKey;
        const h = left && handleAt(e.offsetX, e.offsetY);
        if (pointers.size === 2) { mode = "pan"; if (grab && grab.fresh) sel = null; grab = null; }
        else if (h) { mode = "side"; sel = area(); grab = { h }; }
        else if (left && inside(e.offsetX, e.offsetY)) { mode = "move"; sel = area(); grab = { at: toWorld(e.offsetX, e.offsetY), from: [...sel] }; }
        else if (left) {
            mode = "sel";
            const p = toWorld(e.offsetX, e.offsetY);
            sel = [...p, ...p];
            grab = { fresh: true };
        } else mode = "pan";
        const pts = [...pointers.values()];
        last = pts.reduce((a, q) => [a[0] + q[0] / pts.length, a[1] + q[1] / pts.length], [0, 0]);
        showArea();
        draw();
    });
    canvas.addEventListener("pointermove", (e) => {
        if (!pointers.has(e.pointerId)) {
            canvas.style.cursor = handleAt(e.offsetX, e.offsetY) ? "pointer" : inside(e.offsetX, e.offsetY) ? "move" : "crosshair";
            return;
        }
        pointers.set(e.pointerId, [e.offsetX, e.offsetY]);
        const p = toWorld(e.offsetX, e.offsetY);
        if (mode === "sel") sel = [sel[0], sel[1], ...p];
        else if (mode === "side") {
            const { fx, fz } = grab.h;
            if (fx === 0) sel[0] = Math.min(p[0], sel[2]);
            if (fx === 1) sel[2] = Math.max(p[0] - 1, sel[0]);
            if (fz === 0) sel[1] = Math.min(p[1], sel[3]);
            if (fz === 1) sel[3] = Math.max(p[1] - 1, sel[1]);
        } else if (mode === "move") {
            const dx = p[0] - grab.at[0], dz = p[1] - grab.at[1], f = grab.from;
            sel = [f[0] + dx, f[1] + dz, f[2] + dx, f[3] + dz];
        } else if (mode === "pan") {
            const pts = [...pointers.values()];
            const cur = pts.length === 1 ? pts[0] : pts.reduce((a, q) => [a[0] + q[0] / pts.length, a[1] + q[1] / pts.length], [0, 0]);
            cx -= (cur[0] - last[0]) / zoom;
            cy -= (cur[1] - last[1]) / zoom;
            last = cur;
        }
        showArea();
        draw();
    });
    const up = (e) => {
        pointers.delete(e.pointerId);
        if (!pointers.size) {
            mode = null;
            grab = null;
            if (sel) { sel = area(); hlKey = ""; draw(); }
            loadNext();
        } else {
            const pts = [...pointers.values()];
            last = pts.length === 1 ? pts[0] : pts.reduce((a, q) => [a[0] + q[0] / pts.length, a[1] + q[1] / pts.length], [0, 0]);
        }
    };
    canvas.addEventListener("pointerup", up);
    canvas.addEventListener("pointercancel", up);
    canvas.addEventListener("wheel", (e) => {
        e.preventDefault();
        const before = [cx + (e.offsetX - W / 2) / zoom, cy + (e.offsetY - H / 2) / zoom];
        zoom = Math.min(4, Math.max(0.03, zoom * Math.exp(-e.deltaY * 0.0015)));
        cx = before[0] - (e.offsetX - W / 2) / zoom;
        cy = before[1] - (e.offsetY - H / 2) / zoom;
        draw();
        clearTimeout(wheelT);
        wheelT = setTimeout(loadNext, 150);
    }, { passive: false });
    let wheelT = 0;
    ymin.oninput = () => { cutMoved = true; showArea(); draw(); };

    function end() {
        dead = true;
        box.remove();
        document.body.classList.remove("picking");
        removeEventListener("resize", resize);
        removeEventListener("keydown", key);
        for (const u of urls) URL.revokeObjectURL(u);
    }
    const key = (e) => { if (e.key === "Escape") end(); };
    close.onclick = end;
    go.onclick = () => {
        const [x0, z0, x1, z1] = area();
        end();
        onBake([x0, z0, x1, z1, +ymin.value]);
    };
    addEventListener("resize", resize);
    addEventListener("keydown", key);
    resize();
    showArea();

    backend.regions(world.id).then((r) => {
        if (r.error) { status.textContent = r.error; return; }
        regions = r.regions.map(([rx, rz]) => ({ rx, rz })).sort((a, b) => a.rx + a.rz - (b.rx + b.rz) || a.rx - b.rx);
        draw();
        loadNext();
    });
}
