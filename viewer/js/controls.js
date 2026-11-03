/*
 * Moving about: orbit (three's OrbitControls) with the zoom bar, WASD and the
 * on-screen pad, fly mode, and the sun's keys.
 */
import * as THREE from "three";
import { PAUSE_ICON, PLAY_ICON } from "./viewer.js";

export function setupControls(app) {
    const { aimLight, camera, controls, fitFog, mapDir, placeSun, q, renderer, requestDraw, scene, sky } = app;
    function viewChanged() { fitFog(); requestDraw(); syncBar(); }
    controls.addEventListener("change", viewChanged);

    /* Zoom bar: camera distance to the orbit target on a log scale, top = near. */
    const MIN_D = 4, MAX_D = mapDir ? 4800 : 1200;
    controls.minDistance = MIN_D;
    controls.maxDistance = MAX_D;
    const zbar = app.el("zbar");
    const LOG_SPAN = Math.log(MAX_D / MIN_D);
    function syncBar() {
        const d = camera.position.distanceTo(controls.target);
        zbar.value = String(Math.round(1000 * (1 - Math.log(d / MIN_D) / LOG_SPAN)));
    }
    function setDistance(d) {
        d = Math.min(MAX_D, Math.max(MIN_D, d));
        const off = camera.position.clone().sub(controls.target).setLength(d);
        camera.position.copy(controls.target).add(off);
        controls.update();
    }
    zbar.addEventListener("input", () => setDistance(MIN_D * Math.exp((1 - zbar.value / 1000) * LOG_SPAN)));
    const zoomBy = f => setDistance(camera.position.distanceTo(controls.target) * f);
    app.el("zin").addEventListener("click", () => zoomBy(0.75));
    app.el("zout").addEventListener("click", () => zoomBy(1 / 0.75));
    syncBar();

    /* Movement: forward/back/left/right along the ground relative to the view,
     * up/down along y; camera and target move together, faster when zoomed out. */
    const KEYS = { KeyW: "f", ArrowUp: "f", KeyS: "b", ArrowDown: "b", KeyA: "l", ArrowLeft: "l",
                   KeyD: "r", ArrowRight: "r", KeyE: "u", KeyQ: "d" };
    const FLY_KEYS = { Space: "u", ShiftLeft: "d", ShiftRight: "d" };
    const held = new Set();
    let fast = false, moving = false, last = -1;
    const fwd = new THREE.Vector3(), right = new THREE.Vector3(), step = new THREE.Vector3();
    function tick(t) {
        if (!held.size) {
            moving = false;
            /* the target moved along with the camera, so it may now be in the
               air or inside a hill: the orbit and the zoom would turn about
               nothing on screen */
            if (!app.flying && last >= 0) seatTarget(camera.position.distanceTo(controls.target));
            return;
        }
        /* rAF times can precede the press, so the first frame only starts the clock */
        const dt = last < 0 ? 0 : Math.min(0.25, (t - last) / 1000);
        last = t;
        camera.getWorldDirection(fwd);
        fwd.y = 0;
        if (fwd.lengthSq() < 1e-6) fwd.set(0, 0, -1).applyQuaternion(camera.quaternion).setY(0);
        fwd.normalize();
        right.crossVectors(fwd, camera.up).normalize();
        step.set(0, 0, 0);
        if (held.has("f")) step.add(fwd);
        if (held.has("b")) step.sub(fwd);
        if (held.has("r")) step.add(right);
        if (held.has("l")) step.sub(right);
        if (held.has("u")) step.y += 1;
        if (held.has("d")) step.y -= 1;
        if (step.lengthSq() > 0 && app.flying) {
            step.set(0, 0, 0);
            camera.getWorldDirection(fwd);
            if (held.has("f")) step.add(fwd);
            if (held.has("b")) step.sub(fwd);
            if (held.has("r")) step.add(right);
            if (held.has("l")) step.sub(right);
            if (held.has("u")) step.y += 1;
            if (held.has("d")) step.y -= 1;
            if (step.lengthSq() > 0) {
                camera.position.addScaledVector(step.normalize(), flySpeed * dt);
                flyTarget();
            }
        } else if (step.lengthSq() > 0) {
            const speed = Math.max(10, camera.position.distanceTo(controls.target)) * 0.7 * (fast ? 3 : 1);
            step.normalize().multiplyScalar(speed * dt);
            camera.position.add(step);
            controls.target.add(step);
            controls.update();
        }
        requestAnimationFrame(tick);
    }
    function press(k) {
        held.add(k);
        if (!moving) { moving = true; last = -1; requestAnimationFrame(tick); }
    }
    app.on(app.keys, "keydown", e => {
        fast = e.shiftKey;
        const k = KEYS[e.code] || (app.flying && FLY_KEYS[e.code]);
        if (!k || e.ctrlKey || e.metaKey || e.altKey) return;
        e.preventDefault();
        press(k);
    });
    app.on(app.keys, "keyup", e => {
        fast = e.shiftKey;
        const k = KEYS[e.code] || FLY_KEYS[e.code];
        if (k) held.delete(k);
    });
    app.on(window, "blur", () => held.clear());
    app.on(app.root, "blur", () => held.clear());
    for (const b of app.el("pad").querySelectorAll("button")) {
        const k = b.dataset.k, up = () => { held.delete(k); b.classList.remove("on"); };
        b.addEventListener("pointerdown", e => {
            b.classList.add("on");
            press(k);
            try { b.setPointerCapture(e.pointerId); } catch {}
        });
        b.addEventListener("pointerup", up);
        b.addEventListener("pointercancel", up);
    }
    /* ---- fly mode ----
     * The camera turns about itself (yaw, then pitch) and moves where it
     * looks. controls.target is kept ahead of it at the distance the orbit
     * had (flyD), so what reads the target (haze, the shadow map's box, the
     * tilt-shift) carries on as if orbiting from there: switching modes
     * changes nothing on screen. */
    const LOOK = 0.0025, DRAG_LOOK = 0.005;
    app.flying = false;
    let yaw = 0, pitch = 0, flySpeed = 20, flyD = 100;
    const flyBtn = app.el("fly"), hintEl = app.el("hint");
    let hintTimer = 0;
    function hint(msg, ms = 2500) {
        hintEl.textContent = msg;
        hintEl.classList.add("on");
        clearTimeout(hintTimer);
        hintTimer = setTimeout(() => hintEl.classList.remove("on"), ms);
    }
    function flyTarget() {
        camera.getWorldDirection(fwd);
        controls.target.copy(camera.position).addScaledVector(fwd, flyD);
        viewChanged();
    }
    function lockPointer() {
        try { const p = renderer.domElement.requestPointerLock(); if (p && p.catch) p.catch(() => {}); } catch {}
    }
    function syncLook() {
        camera.getWorldDirection(fwd);
        yaw = Math.atan2(-fwd.x, -fwd.z);
        pitch = Math.asin(THREE.MathUtils.clamp(fwd.y, -1, 1));
        look(0, 0);
    }
    function look(dx, dy) {
        yaw -= dx;
        pitch = THREE.MathUtils.clamp(pitch - dy, -1.55, 1.55);
        camera.rotation.set(pitch, yaw, 0, "YXZ");
        flyTarget();
    }
    /* Distance to what the screen's middle pixel shows: a 1x1 depth render,
     * since the tiles' vertex arrays are gone once uploaded. */
    const pickRT = new THREE.WebGLRenderTarget(1, 1);
    const pickMat = new THREE.MeshDepthMaterial({ depthPacking: THREE.RGBADepthPacking });
    const pickPx = new Uint8Array(4);
    function pickCentre() {
        const fog = scene.fog, cc = renderer.getClearColor(new THREE.Color()), ca = renderer.getClearAlpha();
        scene.fog = null; sky.visible = false; scene.overrideMaterial = pickMat;
        renderer.setClearColor(0xffffff, 1);
        camera.setViewOffset(app.width, app.height, Math.floor(app.width / 2), Math.floor(app.height / 2), 1, 1);
        renderer.setRenderTarget(pickRT);
        renderer.render(scene, camera);
        renderer.readRenderTargetPixels(pickRT, 0, 0, 1, 1, pickPx);
        renderer.setRenderTarget(null);
        camera.clearViewOffset();
        renderer.setClearColor(cc, ca);
        scene.fog = fog; sky.visible = true; scene.overrideMaterial = null;
        if (pickPx[0] === 255 && pickPx[1] === 255 && pickPx[2] === 255 && pickPx[3] === 255) return Infinity;
        /* RGBADepthPacking (three r160): a is the most significant byte */
        const z = pickPx[3] / 256 + pickPx[2] / 65536 + pickPx[1] / 16777216 + pickPx[0] / 4294967296;
        const n = camera.near, f = camera.far;
        return n * f / (f - z * (f - n));
    }
    /* Orbit what is in the middle of the screen, or where the view meets the
     * water's level, or what is d0 ahead: the camera stays, the target moves
     * to it, and the haze and the tilt-shift ease from d0 to the new
     * distance. */
    function seatTarget(d0) {
        camera.getWorldDirection(fwd);
        let t = pickCentre();
        if (!(t < MAX_D)) t = fwd.y < -0.05 ? Math.min(MAX_D, (camera.position.y - app.rayU.uMistY.value) / -fwd.y) : 0;
        if (!(t > 0)) t = d0;
        controls.target.copy(camera.position).addScaledVector(fwd, Math.max(MIN_D, t));
        controls.enabled = true;
        controls.update();
        app.lookFrom = d0;
        app.lookT0 = performance.now();
        const ease = () => {
            if (app.dead) return;
            viewChanged();
            if (app.lookT0 >= 0) requestAnimationFrame(ease);
        };
        ease();
    }
    function setFly(on) {
        if (on === app.flying) return;
        if (on) flyD = THREE.MathUtils.clamp(camera.position.distanceTo(controls.target), MIN_D, MAX_D);
        app.flying = on;
        held.clear();
        app.root.classList.toggle("fly", on);
        flyBtn.classList.toggle("on", on);
        camera.getWorldDirection(fwd);
        if (on) {
            controls.enabled = false;
            syncLook();
            if (matchMedia("(pointer: fine)").matches) {
                lockPointer();
                hint(`fly: click to look with the mouse, Esc to let go; W flies, Space/Shift up/down, wheel ${flySpeed} blocks/s`, 5000);
            } else hint("fly: drag to look, the pad moves");
        } else {
            if (document.pointerLockElement) document.exitPointerLock();
            seatTarget(flyD);
            hint("orbit");
        }
    }
    flyBtn.addEventListener("click", () => { flyBtn.blur(); setFly(!app.flying); });
    app.on(app.keys, "keydown", e => {
        if (e.code === "KeyV" && !e.ctrlKey && !e.metaKey && !e.altKey) setFly(!app.flying);
    });
    const cvs = renderer.domElement;
    app.on(document, "mousemove", e => {
        if (app.flying && document.pointerLockElement === cvs) look(e.movementX * LOOK, e.movementY * LOOK);
    });
    /* without the mouse captured, or by touch: drag to look, a click (with the
       mouse) captures it; touch drags the scene, the mouse turns the view */
    let drag = null;
    cvs.addEventListener("pointerdown", e => {
        if (!app.flying || document.pointerLockElement === cvs || e.button !== 0) return;
        if (drag) { drag = null; return; } /* a second finger: not a look */
        drag = { id: e.pointerId, x: e.clientX, y: e.clientY, moved: 0, touch: e.pointerType !== "mouse" };
        try { cvs.setPointerCapture(e.pointerId); } catch {}
    });
    cvs.addEventListener("pointermove", e => {
        if (!drag || e.pointerId !== drag.id) return;
        const dx = e.clientX - drag.x, dy = e.clientY - drag.y, s = drag.touch ? -DRAG_LOOK : DRAG_LOOK;
        drag.x = e.clientX; drag.y = e.clientY;
        drag.moved += Math.abs(dx) + Math.abs(dy);
        look(dx * s, dy * s);
    });
    const dragEnd = e => {
        if (!drag || e.pointerId !== drag.id) return;
        if (!drag.touch && drag.moved < 4 && e.type === "pointerup") lockPointer();
        drag = null;
    };
    cvs.addEventListener("pointerup", dragEnd);
    cvs.addEventListener("pointercancel", dragEnd);
    cvs.addEventListener("wheel", e => {
        if (!app.flying) return;
        e.preventDefault();
        flySpeed = THREE.MathUtils.clamp(Math.round(flySpeed * (e.deltaY < 0 ? 1.25 : 0.8)), 2, 500);
        hint(`${flySpeed} blocks/s`, 1200);
    }, { passive: false });
    app.on(document, "pointerlockchange", () => {
        if (app.flying && document.pointerLockElement !== cvs) hint("mouse released: click to look again, V to orbit");
    });
    if (q.get("fly") === "1") setFly(true);

    /* the sun: [ and ] or the time-of-day bar move it, T or its button sets
     * it going round, crossing the sky in three minutes and the moonlit night
     * (180-360) in as many. The bar reads as a clock: sunrise (0) at 06:00,
     * sunset (180) at 18:00. */
    const SUN_SPEED = q.has("sunspeed") ? Number(q.get("sunspeed")) : 1;
    let sunRun = 0;
    const sunBar = app.el("sunbar"), sunTime = app.el("sunt"), sunBtn = app.el("sunrun");
    const sunMinutes = deg => ((deg + 90) % 360) * 4;
    function syncSunBar() {
        const m = Math.round(sunMinutes(app.sunDeg)) % 1440;
        sunBar.value = String(m);
        sunTime.textContent = `${String(Math.floor(m / 60)).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}`;
    }
    function moveSun(deg) {
        placeSun(deg);
        aimLight();
        syncSunBar();
        requestDraw();
    }
    function sunTick(ts) {
        if (!sunRun || app.dead) return;
        const dt = Math.min(0.1, (ts - sunRun) / 1000);
        sunRun = ts;
        app.sunMoving = true;
        moveSun(app.sunDeg + dt * SUN_SPEED);
        requestAnimationFrame(sunTick);
    }
    function runSun(on) {
        sunRun = on ? performance.now() : 0;
        sunBtn.classList.toggle("on", on);
        sunBtn.innerHTML = on ? PAUSE_ICON : PLAY_ICON;
        if (on) requestAnimationFrame(sunTick);
        else { app.sunMoving = false; requestDraw(); }
    }
    sunBtn.addEventListener("click", () => { sunBtn.blur(); runSun(!sunRun); });
    sunBar.addEventListener("input", () => {
        if (sunRun) runSun(false);
        moveSun(Number(sunBar.value) / 4 - 90);
    });
    sunBar.addEventListener("change", () => sunBar.blur()); /* the keys go back to moving */
    syncSunBar();
    app.on(app.keys, "keydown", e => {
        if (e.ctrlKey || e.metaKey || e.altKey) return;
        if (e.code === "BracketLeft" || e.code === "BracketRight") moveSun(app.sunDeg + (e.code === "BracketLeft" ? -3 : 3));
        else if (e.code === "KeyT") runSun(!sunRun);
    });
    /* without a redraw (tests); moving: as while it goes round (T) */
    function setSun(deg, moving) { app.sunMoving = !!moving; placeSun(deg); aimLight(); syncSunBar(); }

    /* help: the ? button or key, Esc or × to close */
    const help = app.el("help"), helpBtn = app.el("helpb");
    function setHelp(on) { help.hidden = !on; helpBtn.classList.toggle("on", on); }
    helpBtn.addEventListener("click", () => { helpBtn.blur(); setHelp(help.hidden); });
    app.el("helpx").addEventListener("click", () => setHelp(false));
    app.on(app.keys, "keydown", e => {
        if (e.key === "?") setHelp(help.hidden);
        else if (e.code === "Escape" && !help.hidden) setHelp(false);
    });

    Object.assign(app, { cvs, hint, setFly, setSun, syncBar, syncLook });
}
