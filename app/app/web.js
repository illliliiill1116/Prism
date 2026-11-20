/* The app page's backend when there is no prism serve behind it: prism
   itself runs in the browser (make web), on world folders the user drops
   on the page or picks, and the maps go to the browser's private storage
   (OPFS), where sw.js serves them to the viewer.

   Not the .minecraft folder as a whole: Chrome's folder picker refuses
   anything under AppData. A world dropped or picked (the upload kind of
   folder choice) is read as files, a snapshot for this visit; the game's
   textures come from Mojang's own download server (as the launcher gets
   them), kept in the storage once fetched.

   What to bake and map.json are prism map's own (--plan, --index); only
   the bakes in between are run from here, each in a worker of its own
   (prism-worker.js), as prism map runs a process for each. */

const REGION = /^r\.(-?\d+)\.(-?\d+)\.mca$/;
/* where a world keeps the overworld's region files: 26.x, then older */
const REGION_DIRS = ["dimensions/minecraft/overworld/region/", "region/"];
const MANIFEST = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";

/* prism with args in a worker, see prism-worker.js; its lines go to onLine */
let current = null; /* the bake's run under way: its worker and how to end its wait */
function run(job, onLine, bakes = true) {
    const w = new Worker(new URL("../prism-worker.js", import.meta.url), { type: "module" });
    return new Promise((ok) => {
        const end = (r) => { w.terminate(); if (current && current.w === w) current = null; ok(r); };
        if (bakes) current = { w, end };
        w.onmessage = ({ data }) => {
            if (data.line != null) onLine && onLine(data.line);
            if (data.done) end(data);
        };
        w.onerror = (e) => end({ done: true, code: 1, files: [], error: e.message });
        w.postMessage(job);
    });
}

async function child(dir, ...names) {
    try {
        for (const n of names.slice(0, -1)) dir = await dir.getDirectoryHandle(n);
        const last = names[names.length - 1];
        try { return await dir.getDirectoryHandle(last); } catch { return await dir.getFileHandle(last); }
    } catch { return null; }
}

async function entries(dir) {
    const out = [];
    if (dir) for await (const [name, h] of dir.entries()) out.push([name, h]);
    return out.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0]));
}

async function writeFile(dir, path, data) {
    const parts = path.split("/");
    for (const p of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(p, { create: true });
    const f = await (await dir.getFileHandle(parts[parts.length - 1], { create: true })).createWritable();
    await f.write(data);
    await f.close();
}

async function readText(dir, ...names) {
    const h = await child(dir, ...names);
    return h && h.kind === "file" ? (await h.getFile()).text() : null;
}

/* -------- worlds from what was dropped or picked -------- */

/* A dropped folder's files, as [path, File], reading only what a world
   needs: level.dat, icon.png, the overworld's region files. The folder may
   be a world, the saves folder or .minecraft. */
async function fromEntry(entry) {
    const list = (dir) => new Promise((ok) => {
        const r = dir.createReader(), all = [];
        const more = () => r.readEntries((es) => (es.length ? (all.push(...es), more()) : ok(all)), () => ok(all));
        more();
    });
    const file = (e) => new Promise((ok) => e.file(ok, () => ok(null)));
    const out = [];
    async function world(dir, path) {
        const kids = new Map((await list(dir)).map((e) => [e.name, e]));
        if (!kids.has("level.dat")) return false;
        for (const n of ["level.dat", "icon.png"]) if (kids.has(n)) out.push([path + n, await file(kids.get(n))]);
        for (const rd of REGION_DIRS) {
            let d = dir;
            for (const part of rd.split("/").filter(Boolean)) {
                d = d && (await list(d)).find((e) => e.isDirectory && e.name === part);
            }
            if (!d) continue;
            for (const e of await list(d)) if (e.isFile && REGION.test(e.name)) out.push([path + rd + e.name, await file(e)]);
            break;
        }
        return true;
    }
    if (!entry.isDirectory) return out;
    const root = entry.name + "/";
    if (await world(entry, root)) return out;
    /* the saves folder, or .minecraft (then only its saves) */
    let saves = entry, base = root;
    const kids = await list(entry);
    const s = kids.find((e) => e.isDirectory && e.name === "saves");
    if (s) { saves = s; base = root + "saves/"; }
    for (const e of s ? await list(s) : kids) if (e.isDirectory) await world(e, base + e.name + "/");
    return out;
}

/* [path, File] -> worlds: { id, files: Map(path in the world -> File) } */
function groupWorlds(files) {
    const roots = files.filter(([p]) => p.endsWith("/level.dat")).map(([p]) => p.slice(0, -"level.dat".length));
    const worlds = [];
    for (const root of roots) {
        const id = root.split("/").filter(Boolean).pop();
        const mine = new Map();
        for (const [p, f] of files) if (f && p.startsWith(root) && !roots.some((r) => r !== root && r.startsWith(root) && p.startsWith(r))) mine.set(p.slice(root.length), f);
        worlds.push({ id, files: mine });
    }
    return worlds;
}

/* a world's region files, by name, from the folder it keeps them in */
function regionsOf(world) {
    for (const rd of REGION_DIRS) {
        const m = new Map();
        for (const [p, f] of world.files) if (p.startsWith(rd) && REGION.test(p.slice(rd.length))) m.set(p.slice(rd.length), f);
        if (m.size) return m;
    }
    return new Map();
}

/* an area's clip, its corners in order; and the map it makes, named as
   prism serve names it (serve.c's api_bake) */
export const sortClip = ([x0, z0, x1, z1, y]) => [Math.min(x0, x1), Math.min(z0, z1), Math.max(x0, x1), Math.max(z0, z1), y];
export const areaId = (id, c) => `${id} ${c[0]},${c[1]} to ${c[2]},${c[3]}`;

export async function webBackend() {
    const root = await navigator.storage.getDirectory();
    const maps = await root.getDirectoryHandle("maps", { create: true });
    const jars = await root.getDirectoryHandle("jars", { create: true });
    const worlds = new Map(); /* id -> { id, files, info, icon } */
    let job = null, cancelled = false;

    async function addWorlds(files) {
        for (const w of groupWorlds(files)) {
            const dat = w.files.get("level.dat");
            let text = "";
            await run({ args: ["info", "/mc/w"], files: [{ name: "w/level.dat", data: dat, mtime: dat.lastModified }] }, (l) => { text += l; });
            try { w.info = JSON.parse(text); } catch { w.info = {}; }
            w.played = Math.floor(dat.lastModified / 1000);
            const icon = w.files.get("icon.png");
            const old = worlds.get(w.id);
            if (old && old.icon) URL.revokeObjectURL(old.icon);
            w.icon = icon ? URL.createObjectURL(icon) : null;
            worlds.set(w.id, w);
        }
    }

    /* ?test: worlds put in the private storage beforehand (testmc/saves),
       for headless tests, which can't drop a folder */
    if (new URLSearchParams(location.search).has("test")) {
        const files = [];
        const walk = async (dir, path) => {
            for (const [n, h] of await entries(dir))
                if (h.kind === "directory") await walk(h, path + n + "/");
                else files.push([path + n, await h.getFile()]);
        };
        await walk(await root.getDirectoryHandle("testmc", { create: true }), "");
        await addWorlds(files);
    }

    /* The game's client jar for version ver (else the latest release), from
       the storage or else Mojang's download server; progress(fraction) */
    async function gameJar(ver, progress) {
        const have = await child(jars, ver + ".jar");
        if (have) return { ver, file: await have.getFile() };
        const manifest = await (await fetch(MANIFEST)).json();
        const v = manifest.versions.find((x) => x.id === ver) || manifest.versions.find((x) => x.id === manifest.latest.release);
        const cached = await child(jars, v.id + ".jar");
        if (cached) return { ver: v.id, file: await cached.getFile() };
        const client = (await (await fetch(v.url)).json()).downloads.client;
        const r = await fetch(client.url);
        if (!r.ok) throw new Error("can't download the game's textures (" + r.status + ")");
        const reader = r.body.getReader(), parts = [];
        let got = 0;
        for (;;) {
            const { done, value } = await reader.read();
            if (done) break;
            parts.push(value);
            got += value.length;
            progress(got / client.size);
        }
        const blob = new Blob(parts);
        await writeFile(jars, v.id + ".jar", blob);
        return { ver: v.id, file: await (await child(jars, v.id + ".jar")).getFile() };
    }

    async function state() {
        const s = { web: true, cpus: navigator.hardwareConcurrency || 4, mem: -1, mc: "", data: "", worlds: [], jars: [], maps: [], job };
        for (const w of worlds.values())
            s.worlds.push({ id: w.id, name: w.info.name || w.id, version: w.info.version || "", spawn: w.info.spawn || [0, 0],
                            regions: regionsOf(w).size, icon: !!w.icon, played: w.played });
        for (const [ver] of await entries(jars)) if (ver.endsWith(".jar")) s.jars.push({ ver: ver.slice(0, -4), mtime: 0 });
        for (const [id, dir] of await entries(maps)) {
            if (dir.kind !== "directory") continue;
            const mj = await child(dir, "map.json");
            let regions = 0, bytes = 0;
            for (const [n, h] of await entries(dir)) {
                if (h.kind !== "directory" || !/^r\.-?\d+\.-?\d+$/.test(n)) continue;
                regions++;
                for (const [, f] of await entries(h)) if (f.kind === "file") bytes += (await f.getFile()).size;
            }
            let settings = null;
            try { settings = JSON.parse(await readText(dir, "prism.json")); } catch {}
            s.maps.push({ id, ready: !!mj, baked: mj ? Math.floor((await mj.getFile()).lastModified / 1000) : 0, regions, bytes, settings });
        }
        return s;
    }

    /* prism map, as the native one runs it: --plan, a bake a region, --index */
    async function bake({ world: id, jar: ver, near, clip }) {
        if (job && job.running) return { error: "a bake is under way" };
        const world = worlds.get(id);
        if (!world) return { error: "no such world" };
        const regionFiles = new Map();
        for (const [n, f] of regionsOf(world)) regionFiles.set(n, { name: "region/" + n, data: f, mtime: f.lastModified });
        const v = world.info;
        /* an area of the world is a map of its own (as prism serve names it) */
        if (clip) clip = sortClip(clip);
        const mapId = clip ? areaId(id, clip) : id;
        const out = await maps.getDirectoryHandle(mapId, { create: true });
        const mapArgs = ["map", "/mc/game.jar", "/mc/region", "/out"];
        const clipArgs = clip ? ["--clip", clip.join(",")] : [];
        if (clip) mapArgs.push(...clipArgs);
        else if (near >= 0) mapArgs.push("--near", `${v.spawn[0]},${v.spawn[1]},${near}`);
        const all = [...regionFiles.values()];
        /* each bake's index.json, which --plan and --index read */
        const indexes = async () => {
            const pre = [];
            for (const [n, h] of await entries(out)) {
                if (h.kind !== "directory") continue;
                const idx = await child(h, "index.json");
                if (idx) pre.push({ path: `/out/${n}/index.json`, data: await (await idx.getFile()).arrayBuffer() });
            }
            return pre;
        };
        cancelled = false;
        const t0 = Date.now();
        job = { map: mapId, running: true, code: 0, cancelled: false, elapsed: 0, total: 0, done: 0, baked: 0, failed: 0, secs: 0, current: "", warning: "" };
        const tick = setInterval(() => { job.elapsed = Math.round((Date.now() - t0) / 1000); }, 1000);
        const finish = (code) => { clearInterval(tick); job.running = false; job.code = code; job.cancelled = cancelled; job.current = ""; };
        (async () => {
            const g = await gameJar(ver || v.version || "", (f) => { job.download = f; });
            delete job.download;
            const jar = { name: "game.jar", data: g.file, mtime: g.file.lastModified };
            await writeFile(out, "prism.json", JSON.stringify({ world: id, jar: g.ver, near: clip ? -1 : near, ...(clip ? { clip } : {}), name: v.name || id }) + "\n");
            if (cancelled) return finish(1);
            const lines = [];
            let r = await run({ args: [...mapArgs, "--plan"], files: [jar, ...all], preload: await indexes() }, (l) => lines.push(l));
            if (r.code) { job.warning = lines.pop() || "can't read the world"; return finish(1); }
            const plan = lines.map((l) => l.split(" ")).filter((p) => p[0] === "bake" || p[0] === "keep");
            job.total = plan.length;
            job.done = plan.filter((p) => p[0] === "keep").length;
            /* the bake's threads, and the most it starts at once: chunk
               loading starts one a core (window.c) */
            const cores = navigator.hardwareConcurrency || 4, threads = Math.min(cores, 16), pool = Math.min(Math.max(cores, threads), 64);
            for (const [what, sx, sz] of plan) {
                if (what !== "bake") continue;
                if (cancelled) return finish(1);
                const rx = +sx, rz = +sz;
                job.current = `r.${rx}.${rz}`;
                /* the region and the neighbours its margin reaches into */
                const files = [jar];
                for (let dz = -1; dz <= 1; dz++)
                    for (let dx = -1; dx <= 1; dx++) {
                        const n = `r.${rx + dx}.${rz + dz}.mca`;
                        if (regionFiles.has(n)) files.push(regionFiles.get(n));
                    }
                const t = Date.now(), log = [];
                r = await run({ args: ["bake", "/mc/game.jar", "/mc/region", sx, sz, "/out/r", String(threads), ...clipArgs], files, collect: "/out/r", pool },
                    (l) => { log.push(l); if (l.startsWith("warning: ")) job.warning = l; });
                if (cancelled) return finish(1);
                if (r.code) { job.failed++; job.warning = `r.${rx}.${rz}: ${log.slice(-1)[0] || "failed"}`; }
                else {
                    const dir = `r.${rx}.${rz}`;
                    try { await out.removeEntry(dir, { recursive: true }); } catch {}
                    for (const f of r.files) await writeFile(out, `${dir}/${f.path}`, f.data);
                    await writeFile(out, `${dir}.log`, log.join("\n") + "\n");
                    job.baked++;
                    job.secs += (Date.now() - t) / 1000;
                }
                job.done++;
            }
            r = await run({ args: [...mapArgs, "--index"], files: all, preload: await indexes(), collect: "/out" });
            const mj = r.files.find((f) => f.path === "map.json");
            if (mj) await writeFile(out, "map.json", mj.data);
            finish(r.code || !mj ? 1 : job.failed ? 1 : 0);
        })().catch((e) => { job.warning = String(e && e.message || e); finish(1); });
        return { ok: true };
    }

    /* The area picker's (picker.js): a world's regions, and one's picture
       (prism overview), kept in the storage for as long as the region file
       is the same; one at a time, each taking a bake's memory. OVERVIEW
       goes up when prism overview draws differently, so the kept ones are
       drawn again (prism serve goes by prism's own date instead). */
    const OVERVIEW = 4;
    const overviews = await root.getDirectoryHandle("overviews", { create: true });
    let ovChain = Promise.resolve();
    async function overview(id, ver, rx, rz) {
        const world = worlds.get(id);
        const f = world && regionsOf(world).get(`r.${rx}.${rz}.mca`);
        if (!f) return { error: "no such region" };
        const g = await gameJar(ver || world.info.version || "", () => {});
        const key = `${encodeURIComponent(id)}/${g.ver}/r.${rx}.${rz}`, stamp = `${OVERVIEW}-${f.size}-${f.lastModified}`;
        const dir = await overviews.getDirectoryHandle(encodeURIComponent(id), { create: true });
        const vdir = await dir.getDirectoryHandle(g.ver, { create: true });
        const result = async () => {
            const js = JSON.parse(await readText(vdir, `r.${rx}.${rz}.json`));
            if (js.stamp !== stamp) return null;
            const png = js.w ? await child(vdir, `r.${rx}.${rz}.png`) : null, hpng = js.w ? await child(vdir, `r.${rx}.${rz}.h.png`) : null;
            return { ...js, png: png ? URL.createObjectURL(await png.getFile()) : "", hpng: hpng ? URL.createObjectURL(await hpng.getFile()) : "" };
        };
        try { const r = await result(); if (r) return r; } catch {}
        const work = ovChain.then(async () => {
            const files = [{ name: "game.jar", data: g.file, mtime: g.file.lastModified }];
            const rf = regionsOf(world);
            for (let dz = -1; dz <= 1; dz++)
                for (let dx = -1; dx <= 1; dx++) {
                    const n = `r.${rx + dx}.${rz + dz}.mca`;
                    if (rf.has(n)) files.push({ name: "region/" + n, data: rf.get(n), mtime: rf.get(n).lastModified });
                }
            const cores = navigator.hardwareConcurrency || 4, pool = Math.min(Math.max(cores, 16), 64);
            const r = await run({ args: ["overview", "/mc/game.jar", "/mc/region", "/out", String(rx), String(rz), "--threads", String(Math.min(cores, 16))],
                files, collect: "/out", pool }, null, false);
            const got = (n) => r.files.find((x) => x.path === n);
            const js = got(`r.${rx}.${rz}.json`), png = got(`r.${rx}.${rz}.png`), hpng = got(`r.${rx}.${rz}.h.png`);
            if (r.code || !js) throw new Error("can't draw " + key);
            if (png) await writeFile(vdir, `r.${rx}.${rz}.png`, png.data);
            if (hpng) await writeFile(vdir, `r.${rx}.${rz}.h.png`, hpng.data);
            const meta = { ...JSON.parse(new TextDecoder().decode(js.data)), stamp };
            await writeFile(vdir, `r.${rx}.${rz}.json`, JSON.stringify(meta));
        });
        ovChain = work.catch(() => {});
        await work;
        return result();
    }

    return {
        web: true,
        state,
        bake,
        regions: async (id) => {
            const w = worlds.get(id);
            if (!w) return { error: "no such world" };
            return { regions: [...regionsOf(w).keys()].map((n) => n.match(REGION).slice(1, 3).map(Number)) };
        },
        overview,
        async cancel() {
            cancelled = true;
            if (current) current.end({ done: true, code: 1, files: [] });
            return { ok: true };
        },
        /* a drop's folders (DataTransfer items) */
        async addDropped(items) {
            const es = [...items].map((i) => i.webkitGetAsEntry && i.webkitGetAsEntry()).filter(Boolean);
            const files = [];
            for (const e of es) files.push(...(await fromEntry(e)));
            await addWorlds(files);
            return groupWorlds(files).length;
        },
        /* a folder chosen with <input webkitdirectory> */
        async addPicked(fileList) {
            const files = [...fileList].map((f) => [f.webkitRelativePath || f.name, f]);
            await addWorlds(files);
            return groupWorlds(files).length;
        },
        iconUrl: (w) => worlds.get(w.id) && worlds.get(w.id).icon,
        mapUrl: (id) => "viewer/?map=" + encodeURIComponent(new URL("maps/" + id + "/", location.href).pathname),
        logUrl: () => null,
        /* a map gone from the browser's storage (not while it bakes) */
        async remove(id) {
            if (job && job.running && job.map === id) return { error: "it is being baked" };
            await maps.removeEntry(id, { recursive: true });
            if (job && job.map === id) job = null;
            return { ok: true };
        },
        mapBase: (id) => "maps/" + encodeURIComponent(id) + "/",
        viewerFiles: () => fetch("viewer/files.json").then((r) => r.json()),
        canOpenFolder: false,
    };
}
