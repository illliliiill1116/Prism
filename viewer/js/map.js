/*
 * Map mode: a baked world (prism map) loaded region by region as the view
 * comes near, each 64-block tile at the level of detail its distance calls
 * for, decoded off the frame by workers (tile-worker.js).
 */
import * as THREE from "three";
import { CS, FORMAT, TILES } from "./tile.js";

/* A worker's script must come from the page's own site: with the viewer on
   another (a CDN), the worker is a module of the page's that imports it. */
const workerSrc = new URL("./tile-worker.js", import.meta.url);
const workerUrl = workerSrc.origin === location.origin ? workerSrc
    : URL.createObjectURL(new Blob([`import ${JSON.stringify(workerSrc.href)};`], { type: "text/javascript" }));

export function setupMap(app) {
    const { LAYER_OVER, MSAA, PLANT_FADE, RIPPLE, RIPPLES, RIPPLE_MOVE_PX, WAVE, camera, controls, depthMat, depthMats,
            draw, fitFog, info, patchCutout, patchLight, patchWater, q, renderer, requestDraw, rippleU, scene, setFx,
            shadowBox, shadowMeshes, shadowOn, zoomK } = app;

    /* ---- map mode ----
     * Tiles (prism's index.json and tiles.bin, the parts read with Range
     * requests): each a quad list per material, int16 positions in 1/64 block
     * from the region centre; levels 0 and 1 are a shared part and their own
     * (see decodeLevel). */
    const LOD_D = [192, 640];   /* blocks: full detail within the first, no plants within the second */
    const LOD_DROP = 1.4;       /* a finer level is freed past this many times its range */
    /* Quad budget: tiles nearest the camera take their finest level in range
     * while the quads shown (each 4 vertices, 144 bytes of GPU memory) stay
     * within it, the rest a coarser one. A phone or a small-memory device
     * starts lower, and slow frames lower it further (see adaptResolution).
     * ?budget= in millions of quads. */
    const WEAK = (navigator.deviceMemory || 8) <= 4 || matchMedia("(pointer: coarse)").matches;
    const BUDGET_MAX = (q.has("budget") ? Number(q.get("budget")) : WEAK ? 2 : 8) * 1e6, BUDGET_MIN = Math.min(0.6e6, BUDGET_MAX);
    app.quadBudget = BUDGET_MAX;
    const L0_WEIGHT = 1.5;
    const LOADS = 6;            /* tile requests in flight */
    let quadIndex = null;       /* shared quad index pattern, grown as needed */
    function quadIndexFor(nq) {
        if (!quadIndex || quadIndex.count < nq * 6) {
            let n = 1 << 14;
            while (n < nq) n *= 2;
            const a = new Uint32Array(n * 6);
            for (let i = 0; i < n; i++) a.set([i * 4, i * 4 + 3, i * 4 + 1, i * 4 + 1, i * 4 + 3, i * 4 + 2], i * 6);
            quadIndex = new THREE.BufferAttribute(a, 1);
        }
        return quadIndex;
    }
    function regionMaterials(url) {
        const tex = new THREE.TextureLoader().load(url, requestDraw);
        tex.flipY = false;
        tex.colorSpace = THREE.SRGBColorSpace;
        tex.magFilter = THREE.NearestFilter;
        tex.minFilter = THREE.LinearMipmapLinearFilter;
        tex.wrapS = tex.wrapT = THREE.RepeatWrapping;
        /* as GLTFLoader makes them from prism's region.glb */
        const mk = (name, o) => Object.assign(new THREE.MeshBasicMaterial({ map: tex, vertexColors: true, ...o }), { name });
        /* cut-outs one-sided as in Minecraft: a model that is seen from both
           sides (a cross, a vine) has a face for each, and drawing both faces of
           each showed one's mirror image through the other's clear texels */
        const mats = [mk("OPAQUE", {}), mk("MASK", { alphaTest: 0.5 }),
                      mk("BLEND", { transparent: true, depthWrite: false, side: THREE.DoubleSide }),
                      mk("WATER", { transparent: true, depthWrite: false, side: THREE.DoubleSide })];
        for (const m of mats) {
            if (m.name === "WATER") patchWater(m); else patchLight(m, shadowOn, true);
            if (m.name === "MASK" && MSAA) patchCutout(m, true);
        }
        return mats;
    }
    /* A geometry over nq quads. Its bounds are computed now and its arrays
     * dropped once on the GPU: the tile is never read again, only freed. */
    const dropArray = function () { this.array = null; };
    function quadGeometry(pos, uv, col, nq) {
        const g = new THREE.BufferGeometry();
        const pb = new THREE.InterleavedBuffer(pos, 4).onUpload(dropArray);
        g.setAttribute("position", new THREE.InterleavedBufferAttribute(pb, 3, 0));
        g.setAttribute("bb", new THREE.InterleavedBufferAttribute(pb, 1, 3));
        g.setAttribute("uv", new THREE.BufferAttribute(uv, 2, true).onUpload(dropArray));
        const cb = new THREE.InterleavedBuffer(col, CS).onUpload(dropArray);
        g.setAttribute("color", new THREE.InterleavedBufferAttribute(cb, 3, 0, true));
        g.setAttribute("glow", new THREE.InterleavedBufferAttribute(cb, 1, 3, true));
        g.setAttribute("sun", new THREE.InterleavedBufferAttribute(cb, 4, 4, true));
        g.setAttribute("sunwin", new THREE.InterleavedBufferAttribute(cb, 4, 8, true));
        g.setAttribute("blk", new THREE.InterleavedBufferAttribute(cb, 4, 12, true));
        g.setIndex(quadIndexFor(nq));
        g.setDrawRange(0, nq * 6);
        g.computeBoundingSphere();
        return g;
    }
    /* Meshes for one material's quads. Sorted ones (see sortByFacing) become
     * three, for the x, z and y classes, over the same buffers, each drawing
     * only the part of its range that can face the eye (faceCull). */
    function partMeshes(part, mat) {
        const g = quadGeometry(part.pos, part.uv, part.col, part.nq);
        if (!part.count) {
            const o = new THREE.Mesh(g, mat);
            o.userData.quads = part.nq;
            return [o];
        }
        const out = [], c = part.count;
        let q0 = 0;
        for (const [axis, n] of [[0, c[0] + c[1]], [2, c[2] + c[3]], [1, c[4] + c[5] + c[6]]]) {
            if (n) {
                const sub = new THREE.BufferGeometry();
                for (const k in g.attributes) sub.setAttribute(k, g.attributes[k]);
                sub.setIndex(g.index);
                sub.setDrawRange(q0 * 6, n * 6);
                sub.boundingSphere = g.boundingSphere;
                const o = new THREE.Mesh(sub, mat);
                const cs = axis === 1 ? [c[4], c[5], c[6]] : axis === 0 ? [c[0], 0, c[1]] : [c[2], 0, c[3]];
                o.userData = { quads: n, face: { axis, q0, cs, plus: part.planes[axis * 2], minus: part.planes[axis * 2 + 1] } };
                out.push(o);
            }
            q0 += n;
        }
        return out;
    }
    /* Sets a sorted mesh to draw the quads that can face from: from a point
     * eye (in its group's units) when dir is null, else along a direction
     * (the shadow map's, an orthographic view). */
    function faceCull(o, eye, dir) {
        const f = o.userData.face, [np, no, nm] = f.cs;
        const plus = dir ? dir[f.axis] > 0 : eye[f.axis] > f.plus, minus = dir ? dir[f.axis] < 0 : eye[f.axis] < f.minus;
        const a = f.q0 + (plus ? 0 : np), b = f.q0 + np + no + (minus ? nm : 0);
        o.geometry.setDrawRange(a * 6, (b - a) * 6);
    }
    /* A decoded tile as a mesh per material. */
    function tileMeshes(parts, mats) {
        const out = [];
        for (let m = 0; m < 4; m++) if (parts[m].nq) out.push(...partMeshes(parts[m], mats[m]));
        return out;
    }
    /* Decoded tiles as one mesh per material. */
    function mergedMeshes(tiles, mats) {
        const out = [];
        for (let m = 0; m < 4; m++) {
            let total = 0;
            for (const t of tiles) total += t[m].nq;
            if (!total) continue;
            const nv = total * 4, pos = new Int16Array(nv * 4), uv = new Uint16Array(nv * 2), col = new Uint8Array(nv * CS);
            const sorted = !!tiles[0][m].count, part = { nq: total, pos, uv, col };
            if (sorted) {
                part.count = [0, 0, 0, 0, 0, 0, 0];
                part.planes = [Infinity, -Infinity, Infinity, -Infinity, Infinity, -Infinity];
                for (const t of tiles)
                    for (let k = 0; k < 7; k++) {
                        part.count[k] += t[m].count[k];
                        if (k < 6) part.planes[k] = (k & 1 ? Math.max : Math.min)(part.planes[k], t[m].planes[k]);
                    }
            }
            /* class by class across the tiles, keeping the sort */
            let v = 0;
            for (let k = 0; k < (sorted ? 7 : 1); k++)
                for (const t of tiles) {
                    const { nq, pos: tp, uv: tu, col: tc } = t[m];
                    let a = 0, n = nq;
                    if (sorted) { for (let j = 0; j < k; j++) a += t[m].count[j]; n = t[m].count[k]; }
                    pos.set(tp.subarray(a * 16, (a + n) * 16), v * 4); uv.set(tu.subarray(a * 8, (a + n) * 8), v * 2);
                    col.set(tc.subarray(a * 4 * CS, (a + n) * 4 * CS), v * CS);
                    v += n * 4;
                }
            out.push(...partMeshes(part, mats[m]));
        }
        return out;
    }
    /* water and glass are drawn over the rest (SplitRenderPass) */
    function layerOf(o) { if (o.material.name === "WATER" || o.material.name === "BLEND") o.layers.set(LAYER_OVER); }
    function addMeshes(t, lod, meshes) {
        t.lod[lod] = meshes;
        for (const o of meshes) layerOf(o);
        /* where its water is (for rippleNear), while the positions are still here */
        t.region.group.updateMatrixWorld();
        for (const o of meshes)
            if (o.material.name === "WATER") {
                o.geometry.computeBoundingBox();
                const b = o.geometry.boundingBox.clone().applyMatrix4(t.region.group.matrixWorld);
                meshes.water = meshes.water ? meshes.water.union(b) : b;
            }
        for (const o of meshes) {
            o.visible = false;
            t.region.group.add(o);
            shadowMeshes.set(o, depthMat(o.material));
        }
    }
    function dropMeshes(t, lod) {
        for (const o of t.lod[lod]) {
            t.region.group.remove(o);
            shadowMeshes.delete(o);
            o.geometry.dispose();
        }
        t.lod[lod] = null;
    }
    /* Tiles are fetched, inflated and decoded by workers, off the frame
       (see workerUrl). */
    const tileWorkers = [], tileJobs = new Map();
    let tileJobId = 0;
    /* parts of region's tiles file, [offset, length] each: level 2's pack
       (pack), or a level as [shared part, its own] (either null) */
    function loadTile(region, parts, pack = false) {
        if (!tileWorkers.length) {
            for (let i = 0; i < Math.min(4, Math.max(1, (navigator.hardwareConcurrency || 2) - 1)); i++) {
                const w = new Worker(workerUrl, { type: "module" });
                w.onmessage = e => {
                    if (app.dead) return;
                    const job = tileJobs.get(e.data.id);
                    tileJobs.delete(e.data.id);
                    if (e.data.err) { job.reject(new Error(e.data.err)); return; }
                    /* tiles.bin baked again since the region's index (prism map
                       swaps a region's folder whole): its offsets are not this
                       file's (see tileFailed) */
                    const { mod } = e.data, r = job.region;
                    if (mod && r.mod && mod !== r.mod) { job.reject(Object.assign(new Error(`${r.base}: baked again`), { stale: true })); return; }
                    if (mod && r.state !== "out") r.mod = mod;
                    /* the server sent the whole file (no ranges): later parts come from it */
                    if (e.data.whole && r.state !== "out") r.whole = e.data.whole;
                    /* bytes over the wire and inflated, for tests */
                    if (app.map) { app.map.wire += e.data.sizes.wire; app.map.inflated += e.data.sizes.inflated; }
                    job.resolve(e.data.res);
                };
                tileWorkers.push(w);
            }
        }
        const id = ++tileJobId, url = new URL(`${region.base}/${TILES}${region.ver}`, document.baseURI).href;
        /* a tile's levels to the same worker, which keeps their shared part */
        const key = !pack && parts[0] ? `${url}@${parts[0][0]}` : null;
        let w = id;
        if (key) { w = 0; for (const c of key) w = (w * 31 + c.charCodeAt(0)) | 0; w >>>= 0; }
        const msg = { id, url, parts, key, pack };
        if (region.whole) msg.bufs = parts.map(p => p && region.whole.slice(p[0], p[0] + p[1]));
        return new Promise((resolve, reject) => {
            tileJobs.set(id, { resolve, reject, region });
            tileWorkers[w % tileWorkers.length].postMessage(msg, msg.bufs ? msg.bufs.filter(Boolean) : []);
        });
    }
    function stopTiles() {
        for (const w of tileWorkers) w.terminate();
        tileWorkers.length = 0;
        tileJobs.clear();
    }
    /* What went wrong, for the info line: a fetch from another site that
       does not allow it fails as a bare TypeError. */
    function why(url, e) {
        const from = new URL(url, document.baseURI).origin;
        if (e instanceof TypeError && from !== location.origin) return `${e.message} (${from} must send Access-Control-Allow-Origin)`;
        return String(e && e.message || e);
    }
    async function getJson(url) {
        const r = await fetch(url, { cache: "no-cache" });
        if (!r.ok) throw new Error(`${url}: ${r.status}`);
        return r.json();
    }
    /* A tile that would not load: a region baked again since its index was
     * read is loaded again (from its new index), otherwise the tile is tried
     * again after a while, not on every frame. */
    const RETRY_S = [2, 60]; /* seconds before a failed region or tile is tried again: first, at most */
    function later(n) {
        const ms = Math.min(RETRY_S[0] * 2 ** n, RETRY_S[1]) * 1000;
        setTimeout(requestDraw, ms + 50);
        return performance.now() + ms;
    }
    async function tileFailed(t, lod, e) {
        const r = t.region;
        t.fails[lod]++;
        t.retry[lod] = later(t.fails[lod] - 1);
        if (r.state !== "in") return;
        /* one look at the index for the failures of a few seconds */
        if (!e.stale && !(r.checked > performance.now() - 10000)) {
            r.checked = Infinity;
            r.check = getJson(`${r.base}/index.json`).then(idx => "?" + idx.ver !== r.ver, () => false)
                .finally(() => { r.checked = performance.now(); });
        }
        const rebaked = e.stale || await r.check;
        if (rebaked && r.state === "in") { console.info(`${r.base}: baked again, loading it again`); unloadRegion(r); }
        else if (!rebaked) console.warn(`${r.base}: ${e.message}`);
        requestDraw();
    }
    /* Regions load as they come within sight, the fog's far end (anything
     * past it is fog colour alone): index, atlas and level 2. Past LOD_DROP
     * times that they are freed again. map.json lists them with their tile
     * counts (none: nothing baked there, never fetched) and the sums of the
     * tiles' tx, tz, for where the view starts. */
    const REGION_LOADS = 2; /* regions loading at once */
    async function loadMap(dir) {
        try {
            /* map.json and index.json always revalidate: a browser may otherwise
               keep an old index (and so an old atlas) against a new bake. Tiles
               carry the atlas's version (its content hash), so a cached tile
               only ever comes with the atlas it was baked against (and from the
               same bake, with index.json's "ver"). */
            const m = await getJson(`${dir}/map.json`);
            if (m.format !== FORMAT) throw new Error(stale(m.format));
            app.map = { tiles: [], tilesVer: 0, regions: [], inflight: 0, loading: 0, wire: 0, inflated: 0, centre: new THREE.Vector3() };
            let w = 0;
            for (const r of m.regions) {
                if (r.tiles === 0) continue;
                app.map.regions.push({ base: `${dir}/${r.dir}`, x0: r.rx * 512, z0: r.rz * 512, state: "out", tiles: [], merged: [], mergedOn: false });
                /* the mean of the tiles' centres (64 blocks each), or of the regions' */
                if (r.tx === undefined) { app.map.centre.x += r.rx * 512 + 256; app.map.centre.z += r.rz * 512 + 256; w++; continue; }
                app.map.centre.x += (r.rx * 512 + 32) * r.tiles + 64 * r.tx; app.map.centre.z += (r.rz * 512 + 32) * r.tiles + 64 * r.tz; w += r.tiles;
            }
            /* the mist lies on the map's water, and the view starts just above it */
            const sea = typeof m.sea === "number" ? m.sea : app.MIST_Y;
            app.rayU.uMistY.value = sea;
            app.map.centre.divideScalar(Math.max(1, w)).setY(sea + 2);
            if (!q.has("at")) {
                const off = camera.position.clone().sub(controls.target);
                controls.target.copy(app.map.centre);
                if (!q.has("cam")) off.multiplyScalar(2.5);
                camera.position.copy(app.map.centre).add(off);
            }
            controls.update();
            if (app.flying) app.syncLook();
            fitFog();
            app.syncBar();
            draw();
            setFx(app.fx);
        } catch (e) {
            info.textContent = "map load failed: " + why(dir, e);
            info.classList.add("on");
            app.ready = "error: " + e;
        }
    }
    const stale = f => `the map is in ${f ? "format " + f : "an older format"}, this viewer reads format ${FORMAT}: bake it again`;
    async function loadRegion(region) {
        region.state = "loading";
        app.map.loading++;
        try {
            const { base } = region;
            const idx = await getJson(`${base}/index.json`);
            if (idx.format !== FORMAT) throw Object.assign(new Error(stale(idx.format)), { final: true });
            region.ver = "?" + idx.ver;
            region.group = new THREE.Group();
            region.mats = regionMaterials(`${base}/${idx.atlas}`);
            region.group.position.set(idx.origin[0], 0, idx.origin[1]);
            region.group.scale.setScalar(1 / 64);
            region.group.updateMatrixWorld(true);
            const byXZ = new Map();
            /* tx, tz, quads per level and in the shared part, then the offset
               in tiles.bin and the lengths of level 0's part, the shared part
               and level 1's */
            for (const [tx, tz, q0, q1, q2, qs, ...at] of idx.tiles)
                byXZ.set(`${tx},${tz}`, { region, tx, tz, quads: [q0, q1, q2, qs], at, lod: [null, null, null], want: [false, false],
                                          fails: [0, 0], retry: [0, 0], shown: -1,
                                          x: idx.origin[0] - 256 + tx * idx.tile + idx.tile / 2,
                                          z: idx.origin[1] - 256 + tz * idx.tile + idx.tile / 2, half: idx.tile / 2 });
            region.tiles = [...byXZ.values()];
            /* level 2 for the whole region in one go: per tile, and merged
               into one mesh per material for when all of them are coarse
               (a whole map is otherwise thousands of draw calls) */
            const decoded = [];
            for (const { tx, tz, parts } of await loadTile(region, [idx.l2], true)) {
                const t = byXZ.get(`${tx},${tz}`);
                if (t) { addMeshes(t, 2, tileMeshes(parts, region.mats)); decoded.push(parts); }
            }
            for (const t of byXZ.values()) if (!t.lod[2]) t.lod[2] = [];
            region.merged = mergedMeshes(decoded, region.mats);
            region.mergedOn = false;
            for (const o of region.merged) { o.visible = false; layerOf(o); region.group.add(o); shadowMeshes.set(o, depthMat(o.material)); }
            scene.add(region.group);
            app.map.tiles.push(...region.tiles);
            app.map.tilesVer++;
            region.state = "in";
            region.fails = 0;
        } catch (e) {
            console.warn(region.base, e);
            info.textContent = `${region.base}: ${why(region.base, e)}`;
            info.classList.add("on");
            /* gone a moment (being swapped for a new bake) or cut short: tried
               again later; in a format this viewer does not read: never */
            if (region.mats) unloadRegion(region);
            Object.assign(region, { state: e.final ? "failed" : "out", retry: later((region.fails = (region.fails || 0) + 1) - 1) });
        } finally {
            app.map.loading--;
            requestDraw();
        }
    }
    function unloadRegion(region) {
        for (const t of region.tiles) {
            t.gone = true; /* a level still on its way is dropped when it comes */
            for (let l = 0; l < 3; l++) if (t.lod[l]) dropMeshes(t, l);
        }
        for (const o of region.merged) { shadowMeshes.delete(o); o.geometry.dispose(); }
        scene.remove(region.group);
        for (const m of region.mats) {
            for (const d of depthMats.get(m) || []) if (d) d.dispose();
            depthMats.delete(m);
            m.dispose();
        }
        region.mats[0].map.dispose();
        app.map.tiles = app.map.tiles.filter(t => t.region !== region);
        app.map.tilesVer++;
        Object.assign(region, { state: "out", tiles: [], merged: [], mergedOn: false, group: null, mats: null, whole: null, mod: null,
                                check: null, checked: 0 });
    }
    /* Loads the regions in sight, nearest first, and frees those far past it;
     * returns how many in sight are still to come. */
    function updateRegions() {
        const cam = camera.position, near = scene.fog.far, wanted = [];
        for (const r of app.map.regions) {
            const dx = Math.max(r.x0 - cam.x, cam.x - r.x0 - 512, 0), dz = Math.max(r.z0 - cam.z, cam.z - r.z0 - 512, 0);
            const d = Math.hypot(dx, Math.max(cam.y - 320, -64 - cam.y, 0), dz);
            if (r.state === "out" && d < near && !(r.retry > performance.now())) wanted.push([d, r]);
            else if (r.state === "in" && d > near * LOD_DROP) unloadRegion(r);
        }
        wanted.sort((a, b) => a[0] - b[0]);
        for (const [, r] of wanted) if (app.map.loading < REGION_LOADS) loadRegion(r);
        return wanted.length;
    }
    /* Each tile at the level its distance asks for, the nearest loaded one
     * meanwhile; requests go out nearest first. */
    function updateTiles() {
        const regionsDue = updateRegions();
        const cam = camera.position, now = performance.now();
        const want = [];
        let changed = false, shown = [0, 0, 0], quads = 0, used = 0, held = 0;
        for (const t of app.map.tiles) {
            const dx = Math.max(Math.abs(cam.x - t.x) - t.half, 0), dz = Math.max(Math.abs(cam.z - t.z) - t.half, 0);
            const dy = Math.max(cam.y - 120, 40 - cam.y, 0);
            t.d = Math.hypot(dx, dy, dz);
            t.dl = t.d * zoomK.value;
            used += t.quads[2];
            for (let l = 0; l < 2; l++) if (t.lod[l]) held += t.quads[l];
        }
        /* Upgrades in order of distance, a step to level 0 (plants, a large
           share of the quads) counting as L0_WEIGHT times as far, and none
           where plants have faded out anyway (PLANT_FADE):
           each taken if it fits, one already held a little over, so a tile at
           the margin does not flip */
        /* rebuilt as regions come and go */
        if (app.map.candVer !== app.map.tilesVer) {
            app.map.cand = app.map.tiles.flatMap(t => [{ t, l: 1, k: 0 }, { t, l: 0, k: 0 }]);
            app.map.candVer = app.map.tilesVer;
        }
        for (const c of app.map.cand) { c.k = c.l ? c.t.dl : c.t.dl * L0_WEIGHT; c.t.target = 2; }
        app.map.cand.sort((a, b) => a.k - b.k);
        for (const { t, l } of app.map.cand) {
            if (t.dl >= (l ? LOD_D[l] : Math.min(LOD_D[0], PLANT_FADE[1])) || t.target !== l + 1) continue;
            const cost = t.quads[l] - t.quads[l + 1];
            if (used + cost > app.quadBudget * (t.lod[l] ? 1.1 : 1)) continue;
            used += cost;
            t.target = l;
        }
        const tight = held > app.quadBudget * 1.25;
        for (const t of app.map.tiles) {
            const d = t.dl;
            const lod = t.target;
            for (let l = 0; l < 2; l++)
                if (t.lod[l] && (d > LOD_D[l] * LOD_DROP || (tight && l < lod))) { if (t.shown === l) t.shown = -1; dropMeshes(t, l); changed = true; }
            if (lod < 2 && !t.lod[lod] && !t.want[lod] && t.quads[lod] && !(t.retry[lod] > now)) want.push([d, t, lod]);
            let s = -1;
            for (let l = lod; l >= 0 && s < 0; l--) if (t.lod[l]) s = l; /* finer, if still held */
            for (let l = lod + 1; l < 3 && s < 0; l++) if (t.lod[l]) s = l;
            t.s = s;
            if (s >= 0) shown[s]++;
        }
        /* a region whose tiles are all coarse draws its merged mesh instead */
        for (const r of app.map.regions) {
            if (r.state !== "in") continue;
            const merged = r.tiles.every(t => t.s === 2);
            if (merged !== r.mergedOn) {
                for (const o of r.merged) o.visible = merged;
                r.mergedOn = merged;
                changed = true;
            }
            if (merged) for (const o of r.merged) quads += o.userData.quads;
            for (const t of r.tiles) {
                const s = merged ? -1 : t.s;
                if (s !== t.shown) {
                    if (t.shown >= 0 && t.lod[t.shown]) for (const o of t.lod[t.shown]) o.visible = false;
                    if (s >= 0) for (const o of t.lod[s]) o.visible = true;
                    t.shown = s;
                    changed = true;
                }
                if (s >= 0) for (const o of t.lod[s]) quads += o.userData.quads;
            }
        }
        /* the shadow map holds what was shown when it was drawn */
        if (changed) shadowBox.H = 0;
        want.sort((a, b) => a[0] - b[0]);
        for (const [, t, lod] of want) {
            if (app.map.inflight >= LOADS) break;
            t.want[lod] = true;
            app.map.inflight++;
            /* level 0 is its part then the shared one, level 1 the shared one
               then its part: either one run of bytes */
            const [off, n0, ns, n1] = t.at, own = lod ? [off + n0 + ns, n1] : [off, n0];
            loadTile(t.region, [ns ? [off + n0, ns] : null, own[1] ? own : null]).then(parts => {
                t.fails[lod] = 0;
                if (!t.gone && t.dl <= LOD_D[lod] * LOD_DROP) addMeshes(t, lod, tileMeshes(parts, t.region.mats));
            }).catch(e => { if (!t.gone) return tileFailed(t, lod, e); }).finally(() => { t.want[lod] = false; app.map.inflight--; requestDraw(); });
        }
        app.map.shownQuads = quads;
        app.map.moving = swayNear() || rippleNear();
        info.textContent = `tiles: ${shown[0]} full, ${shown[1]} no plants, ${shown[2]} coarse; ${(quads * 2 / 1e6).toFixed(2)} M triangles` +
            `${app.quadBudget < BUDGET_MAX || WEAK ? ` (budget ${(app.quadBudget * 2 / 1e6).toFixed(1)} M)` : ""}; ${app.map.inflight} loading`;
        app.map.settled = app.map.inflight === 0 && !want.length && !app.map.loading && !regionsDue;
        app.ready = app.map.inflight === 0 && !want.length && !app.map.loading && !regionsDue ? true : app.ready === true ? true : false;
    }

    /* whether a tile drawn with its plants (level 0) is in view within the
       distance their sway shows at (see WAVE) */
    const swayFrustum = new THREE.Frustum(), swayBox = new THREE.Box3(), swayM = new THREE.Matrix4();
    function swayNear() {
        if (!(WAVE > 0)) return false;
        const far = WAVE * renderer.domElement.height / (2 * Math.tan(THREE.MathUtils.degToRad(camera.fov) / 2));
        swayFrustum.setFromProjectionMatrix(swayM.multiplyMatrices(camera.projectionMatrix, camera.matrixWorldInverse));
        for (const t of app.map.tiles) {
            if (t.shown !== 0 || t.d > far) continue;
            swayBox.min.set(t.x - t.half, -64, t.z - t.half);
            swayBox.max.set(t.x + t.half, 320, t.z + t.half);
            if (swayFrustum.intersectsBox(swayBox)) return true;
        }
        return false;
    }
    /* whether water in view is near enough to ripple (see RIPPLE) */
    function rippleNear() {
        if (!(RIPPLE > 0)) return false;
        const far = RIPPLES[0][0] / RIPPLE_MOVE_PX / rippleU.uPixK.value;
        swayFrustum.setFromProjectionMatrix(swayM.multiplyMatrices(camera.projectionMatrix, camera.matrixWorldInverse));
        for (const t of app.map.tiles) {
            const w = t.shown >= 0 && t.lod[t.shown] && t.lod[t.shown].water;
            if (w && w.distanceToPoint(camera.position) <= far && swayFrustum.intersectsBox(w)) return true;
        }
        return false;
    }

    Object.assign(app, { BUDGET_MAX, BUDGET_MIN, faceCull, layerOf, loadMap, regionMaterials, stopTiles, updateTiles });
}
