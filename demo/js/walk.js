/*
 * Walking: in fly mode, G puts the eye on legs. A walker 0.6 blocks wide and
 * 1.8 tall (the eye at 1.62) falls, jumps (Space), steps up half blocks and
 * stairs on its own, runs with Shift or W pressed twice, swims and climbs
 * ladders and vines, moving as Minecraft's player does (see the walker).
 *
 * What it bumps into is the bake's solid.bin (see prism's src/tiles/solid.c):
 * per block the height each quarter of its floor is solid to, and flags for
 * water, lava and climbing. Only the tiles around the walker are read, with
 * Range; until the one it stands in has come, it waits where it is.
 */
import * as THREE from "three";

const W = 0.3, HEIGHT = 1.8, EYE = 1.62, STEP = 0.6;
const C_WATER = 1 << 16, C_LAVA = 1 << 17, C_CLIMB = 1 << 18;
const HEAD = 8, TB = 64; /* solid.bin: "PSOL", version, then 64 x 12-byte entries; tiles of 64 blocks */
const EPS = 1e-4;
const DOUBLE_TAP = 350; /* ms between two presses of W that start a run (the game's 7 ticks) */

export function setupWalk(app) {
    const { camera, q } = app;
    app.walking = false;

    /* ---- solid.bin ---- */
    const files = new Map(); /* region base -> { head: DataView, whole?: ArrayBuffer, tiles: Map } */
    async function fileOf(r) {
        let f = files.get(r.base);
        if (f && f.ver === r.ver) return f;
        f = { ver: r.ver, tiles: new Map(), ready: null };
        files.set(r.base, f);
        f.ready = (async () => {
            const url = `${r.base}/solid.bin${r.ver || ""}`;
            const res = await fetch(url, { headers: { Range: `bytes=0-${HEAD + 64 * 12 - 1}` } });
            if (!res.ok) throw new Error(`${url}: ${res.status}`);
            const buf = await res.arrayBuffer();
            if (res.status === 200) f.whole = buf; /* no ranges: the whole file */
            f.head = new DataView(buf, 0, HEAD + 64 * 12);
            f.url = url;
        })();
        return f;
    }
    async function inflate(buf) {
        const s = new Blob([buf]).stream().pipeThrough(new DecompressionStream("deflate"));
        return new Uint8Array(await new Response(s).arrayBuffer());
    }
    async function loadTile(f, t) {
        await f.ready;
        const e = HEAD + 12 * t, off = f.head.getUint32(e, true), len = f.head.getUint32(e + 4, true);
        const ylo = f.head.getInt16(e + 8, true), rows = f.head.getUint16(e + 10, true);
        if (!len) return { ylo: 0, rows: 0 };
        let buf;
        if (f.whole) buf = f.whole.slice(off, off + len);
        else {
            const res = await fetch(f.url, { headers: { Range: `bytes=${off}-${off + len - 1}` } });
            buf = await res.arrayBuffer();
            if (res.status === 200) { f.whole = buf; buf = buf.slice(off, off + len); }
        }
        const d = await inflate(buf), dv = new DataView(d.buffer);
        const n = dv.getUint16(0, true), pal = new Uint32Array(n);
        for (let k = 0; k < n; k++) pal[k] = dv.getUint32(2 + 4 * k, true);
        return { ylo, rows, pal, cells: d.subarray(2 + 4 * n) };
    }
    let missing = false; /* a block asked for whose tile has not come */
    function regionAt(x, z) {
        const rx = Math.floor(x / 512) * 512, rz = Math.floor(z / 512) * 512;
        return app.map && app.map.regions.find(r => r.x0 === rx && r.z0 === rz);
    }
    function tileAt(x, z) {
        const r = regionAt(x, z);
        if (!r) return null; /* off the map: open air */
        if (r.state !== "in" || !r.solid) { missing = true; return undefined; }
        const f = files.get(r.base);
        const t = Math.floor((z - r.z0) / TB) * 8 + Math.floor((x - r.x0) / TB);
        let tile = f && f.ver === r.ver ? f.tiles.get(t) : undefined;
        if (tile === undefined) {
            missing = true;
            (async () => {
                const ff = await fileOf(r);
                if (ff.tiles.has(t)) return;
                ff.tiles.set(t, null); /* on its way */
                try { ff.tiles.set(t, await loadTile(ff, t)); } catch (e) { console.warn(e); ff.tiles.delete(t); }
            })();
            return undefined;
        }
        if (tile === null) missing = true;
        return tile || undefined;
    }
    function coll(x, y, z) {
        const t = tileAt(x, z);
        if (!t) return 0;
        const r = y - t.ylo;
        if (r < 0 || r >= t.rows) return 0;
        const lx = ((x % TB) + TB) % TB, lz = ((z % TB) + TB) % TB;
        return t.pal[t.cells[(r * TB + lz) * TB + lx]];
    }

    /* ---- the walker ----
     * Minecraft's movement as its players know it (the Minecraft Wiki's
     * "Entity" and "Sprinting" pages): it ticks 20 times a second, and per
     * tick, in blocks and ticks, the keys add an acceleration (0.1 on the
     * ground, 0.02 in the air, 1.3 times that running), it moves, then
     * gravity takes 0.08 off its upward speed, the air keeps 0.98 of that,
     * and of its speed across it keeps 0.546 on the ground (a block's
     * slipperiness 0.6 x 0.91) and 0.91 in the air. That comes to 4.3 blocks
     * a second walking and 5.6 running. A jump is 0.42 up, and running adds
     * 0.2 forward to it. Water keeps 0.8 of the speed, sinks 0.005 a tick,
     * and Space swims up 0.04 a tick; lava keeps 0.5. Ladders and vines hold
     * the speed across to 0.15 and the fall to 0.15, and climb at 0.2 with
     * Space or against a wall. The view is drawn between the last two ticks
     * and widens 15% running, as the game's does (but does not bob). Ice and
     * other slippery blocks are not told apart: all are 0.6. */
    const TICK = 50; /* ms */
    const feet = new THREE.Vector3(), prev = new THREE.Vector3(), vel = new THREE.Vector3();
    let onGround = false, hitWall = false, loop = false, sprint = false, wDown = -1e9, noJump = 0, acc = 0, last = -1;
    let fovMod = 1, prevFovMod = 1;
    /* the boxes of the blocks a box from lo to hi touches, into fn(x0, y0, z0, x1, y1, z1) */
    function boxes(lo, hi, fn) {
        for (let y = Math.floor(lo.y) - 1; y <= Math.floor(hi.y); y++)
            for (let z = Math.floor(lo.z); z <= Math.floor(hi.z); z++)
                for (let x = Math.floor(lo.x); x <= Math.floor(hi.x); x++) {
                    const c = coll(x, y, z) & 0xffff;
                    if (!c) continue;
                    for (let k = 0; k < 4; k++) {
                        const h = (c >> (4 * k)) & 15;
                        if (h) fn(x + (k & 1) * 0.5, y, z + (k >> 1) * 0.5, x + (k & 1) * 0.5 + 0.5, y + h / 15, z + (k >> 1) * 0.5 + 0.5);
                    }
                }
    }
    const lo = new THREE.Vector3(), hi = new THREE.Vector3();
    /* moves feet along axis a (0 x, 1 y, 2 z) by d, short of anything in the way; the distance gone */
    function move(a, d) {
        if (!d) return 0;
        lo.set(feet.x - W, feet.y, feet.z - W);
        hi.set(feet.x + W, feet.y + HEIGHT, feet.z + W);
        const L = [lo.x, lo.y, lo.z], H = [hi.x, hi.y, hi.z];
        if (d > 0) H[a] += d; else L[a] += d;
        boxes(lo.fromArray(L), hi.fromArray(H), (x0, y0, z0, x1, y1, z1) => {
            const b0 = [x0, y0, z0], b1 = [x1, y1, z1];
            for (let o = 0; o < 3; o++) {
                if (o === a) continue;
                const p0 = o === 1 ? feet.y : (o === 0 ? feet.x : feet.z) - W, p1 = o === 1 ? feet.y + HEIGHT : p0 + 2 * W;
                if (p1 <= b0[o] + EPS || p0 >= b1[o] - EPS) return;
            }
            const p0 = a === 1 ? feet.y : (a === 0 ? feet.x : feet.z) - W, p1 = a === 1 ? feet.y + HEIGHT : p0 + 2 * W;
            if (d > 0 && p1 <= b0[a] + EPS) d = Math.min(d, b0[a] - p1);
            else if (d < 0 && p0 >= b1[a] - EPS) d = Math.max(d, b1[a] - p0);
        });
        if (a === 0) feet.x += d; else if (a === 1) feet.y += d; else feet.z += d;
        return d;
    }
    /* flags of the blocks the body is in */
    function inside() {
        let f = 0;
        for (let y = Math.floor(feet.y); y <= Math.floor(feet.y + HEIGHT - 0.2); y++)
            for (let z = Math.floor(feet.z - W); z <= Math.floor(feet.z + W); z++)
                for (let x = Math.floor(feet.x - W); x <= Math.floor(feet.x + W); x++) f |= coll(x, y, z);
        return f;
    }
    function blocked() {
        let hit = false;
        boxes(lo.set(feet.x - W, feet.y, feet.z - W), hi.set(feet.x + W, feet.y + HEIGHT, feet.z + W),
              (x0, y0, z0, x1, y1, z1) => {
                  if (x1 > feet.x - W + EPS && x0 < feet.x + W - EPS && y1 > feet.y + EPS && y0 < feet.y + HEIGHT - EPS &&
                      z1 > feet.z - W + EPS && z0 < feet.z + W - EPS) hit = true;
              });
        return hit;
    }
    /* the move of a tick, d in blocks: up or down first, then across; across
       blocked on the ground, a step up (STEP) and down again */
    function moveBy(d) {
        const gy = move(1, d.y), wasGround = onGround;
        onGround = d.y < 0 && gy > d.y + EPS;
        const sx = feet.x, sy = feet.y, sz = feet.z;
        const gx = move(0, d.x), gz = move(2, d.z);
        if ((wasGround || onGround) && (Math.abs(gx - d.x) > EPS || Math.abs(gz - d.z) > EPS)) {
            const ax = feet.x, ay = feet.y, az = feet.z;
            feet.set(sx, sy, sz);
            const rise = move(1, STEP);
            move(0, d.x); move(2, d.z);
            move(1, -rise);
            if ((feet.x - sx) ** 2 + (feet.z - sz) ** 2 <= (ax - sx) ** 2 + (az - sz) ** 2 + EPS) feet.set(ax, ay, az);
            else onGround = true;
        }
        const cx = Math.abs(feet.x - sx - d.x) > EPS, cz = Math.abs(feet.z - sz - d.z) > EPS;
        hitWall = cx || cz;
        if (gy !== d.y) d.y = 0;
        if (cx) d.x = 0;
        if (cz) d.z = 0;
    }
    const d = new THREE.Vector3();
    function tick() {
        const held = app.held, [yaw] = app.lookAngles();
        prev.copy(feet);
        prevFovMod = fovMod;
        const fl = inside(), water = fl & C_WATER, lava = fl & C_LAVA, climb = fl & C_CLIMB;
        let fwdIn = (held.has("f") ? 1 : 0) - (held.has("b") ? 1 : 0), sideIn = (held.has("r") ? 1 : 0) - (held.has("l") ? 1 : 0);
        if (fwdIn <= 0 || water || lava || (hitWall && fwdIn > 0 && Math.hypot(vel.x, vel.z) < 0.05)) sprint = false;
        const running = sprint || (held.has("d") && fwdIn > 0 && !water && !lava);
        /* the game's jump: on the ground, 10 ticks apart while held */
        if (noJump > 0) noJump--;
        if (Math.abs(vel.x) < 0.003) vel.x = 0;
        if (Math.abs(vel.y) < 0.003) vel.y = 0;
        if (Math.abs(vel.z) < 0.003) vel.z = 0;
        const sy = Math.sin(yaw), cy = Math.cos(yaw);
        if (held.has("u")) {
            if (water || lava) vel.y += 0.04;
            else if (onGround && noJump === 0) {
                vel.y = 0.42;
                if (running) { vel.x += -sy * 0.2; vel.z += -cy * 0.2; }
                noJump = 10;
            }
        } else noJump = 0;
        /* the keys, as an acceleration along the view (yaw: 0 looks down -z) */
        fwdIn *= 0.98; sideIn *= 0.98;
        let n = Math.hypot(fwdIn, sideIn);
        const speed = water || lava ? 0.02 : onGround ? (running ? 0.13 : 0.1) : running ? 0.026 : 0.02;
        if (n > 1e-4) {
            const k = speed / Math.max(1, n);
            const f = fwdIn * k, s = sideIn * k;
            vel.x += -sy * f + cy * s;
            vel.z += -cy * f - sy * s;
        }
        if (climb && !water) {
            vel.x = THREE.MathUtils.clamp(vel.x, -0.15, 0.15);
            vel.z = THREE.MathUtils.clamp(vel.z, -0.15, 0.15);
            vel.y = Math.max(vel.y, -0.15);
        }
        moveBy(d.copy(vel));
        vel.copy(d);
        if (climb && !water && (hitWall || held.has("u"))) vel.y = 0.2;
        if (water) { vel.x *= 0.8; vel.y = vel.y * 0.8 - 0.005; vel.z *= 0.8; }
        else if (lava) { vel.x *= 0.5; vel.y = vel.y * 0.5 - 0.02; vel.z *= 0.5; }
        else {
            const fr = onGround ? 0.6 * 0.91 : 0.91;
            vel.y = (vel.y - 0.08) * 0.98;
            vel.x *= fr; vel.z *= fr;
        }
        /* the view widens while running */
        fovMod += ((running && !water && !lava && Math.hypot(vel.x, vel.z) > 0.01 ? 1.15 : 1) - fovMod) * 0.5;
    }
    const eye = new THREE.Vector3();
    function frame(t) {
        if (!app.walking || app.dead) { loop = false; return; }
        requestAnimationFrame(frame);
        const dt = last < 0 ? 0 : Math.min(250, t - last);
        last = t;
        missing = false;
        inside(); blocked(); /* asks for the tiles round the walker */
        boxes(lo.set(feet.x - 2, feet.y - 3, feet.z - 2), hi.set(feet.x + 2, feet.y + 3, feet.z + 2), () => {});
        if (missing) return; /* wait where it is for its blocks */
        for (acc += dt; acc >= TICK; acc -= TICK) tick();
        if (feet.y < -200) { setWalk(false); app.hint("fell off the map: flying again"); return; }
        /* between the last two ticks */
        const a = acc / TICK, [yaw, pitch] = app.lookAngles();
        eye.lerpVectors(prev, feet, a).y += EYE;
        camera.rotation.set(pitch, yaw, 0, "YXZ");
        const fm = prevFovMod + (fovMod - prevFovMod) * a, half = THREE.MathUtils.degToRad(camera.fov) / 2;
        const zoom = Math.tan(half) / Math.tan(Math.min(1.5, half * fm));
        if (!camera.position.equals(eye) || Math.abs(camera.zoom - zoom) > 1e-5) {
            camera.zoom = zoom;
            camera.updateProjectionMatrix();
            camera.position.copy(eye);
            app.flyTarget();
        }
    }
    /* the walker under the eye, lifted out of anything it stands in */
    function place() {
        feet.set(camera.position.x, camera.position.y - EYE, camera.position.z);
        vel.set(0, 0, 0);
        prev.copy(feet);
        acc = 0;
        for (let i = 0; i < 400 && blocked(); i++) feet.y = Math.floor(feet.y) + 1;
    }
    function setWalk(on) {
        if (on === app.walking) return;
        if (on) {
            if (!app.flying) app.setFly(true);
            const r = regionAt(camera.position.x, camera.position.z);
            if (r && r.state === "in" && !r.solid) { app.hint("this map has no solid.bin: bake it again to walk"); return; }
            app.walking = true;
            last = -1;
            onGround = false;
            const start = () => {
                if (!app.walking) return;
                missing = false;
                place();
                if (missing) { setTimeout(start, 100); return; }
                if (!loop) { loop = true; requestAnimationFrame(frame); }
            };
            start();
            app.hint("walk: WASD, W twice or Shift runs, Space jumps / swims / climbs; G to fly", 4000);
        } else {
            app.walking = false;
            /* the view as flying has it: no running width */
            camera.zoom = 1;
            camera.updateProjectionMatrix();
            const [yaw, pitch] = app.lookAngles();
            camera.rotation.set(pitch, yaw, 0, "YXZ");
            app.flyTarget();
            app.hint("fly");
        }
        app.root.classList.toggle("walk", app.walking);
    }
    /* W twice in quick succession: run, until it is let go */
    app.on(app.keys, "keydown", e => {
        if (!app.walking || e.repeat || (e.code !== "KeyW" && e.code !== "ArrowUp")) return;
        const t = performance.now();
        if (t - wDown < DOUBLE_TAP) sprint = true;
        wDown = t;
    });
    app.on(app.keys, "keydown", e => {
        if (e.code === "KeyG" && !e.ctrlKey && !e.metaKey && !e.altKey && app.map) setWalk(!app.walking);
    });
    /* leaving fly mode leaves walking too */
    const setFly = app.setFly;
    app.setFly = on => { if (!on) setWalk(false); setFly(on); };
    if (q.get("walk") === "1") {
        const wait = () => (app.ready === true ? setWalk(true) : setTimeout(wait, 200));
        wait();
    }
    Object.assign(app, { setWalk, walkFeet: feet, walkColl: coll });
}
