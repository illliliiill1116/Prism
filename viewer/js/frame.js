/*
 * Drawing a frame: at most one per display frame, the pixel ratio adapted
 * to how fast frames come, and the view handed to onView.
 */
import * as THREE from "three";

export function setupFrame(app) {
    const { PR_MAX, PR_MIN, RIPPLE, WAVE, WAVE_FPS, camera, composer, controls, diag, fitTilt, linBlend, rayU,
            renderShadow, renderer, rippleU, scene, shadowBox, shadowMeshes, sizeEffects, sky, stats, tiltH, tiltV,
            waterMats, waveU } = app;
    /* one frame finished on the GPU (?diag&bench, the test hooks), in ms; fresh = with a new shadow map */
    function timeFrame(fresh) {
        if (fresh) shadowBox.H = 0;
        const gl = renderer.getContext(), t = performance.now();
        draw();
        gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(4));
        return performance.now() - t;
    }

    /* at most one draw per display frame, however many events ask for it */
    let drawQueued = false;
    let drawAsked = 0;
    function requestDraw() {
        if (drawQueued || app.shot || app.dead) return;
        drawQueued = true;
        drawAsked = performance.now();
        requestAnimationFrame(() => { drawQueued = false; if (app.shot) return; adaptResolution(performance.now()); draw(); });
    }
    /* Dynamic resolution: while no tiles are loading, how long draws take
       to come is what the GPU keeps up with: the gap since the last one, or,
       after a pause, the wait since this one was asked for (a slow GPU holds
       the next frame back). Over 40 ms (median of 20, of 5 when far over)
       the pixel ratio drops a step, under 22 ms it climbs back, between PR_MIN
       and PR_MAX and at most every 1.5 s. Most views are bound by vertices,
       not pixels, so at PR_MIN the quad budget drops instead (to as little as
       0.3 of what is shown), and comes back before the pixel ratio does. */
    const frameGaps = [];
    let lastFrameTs = 0, lastPrChange = 0;
    function adaptResolution(ts) {
        const gap = Math.min(ts - lastFrameTs, ts - drawAsked);
        lastFrameTs = ts;
        /* tiles arriving make slow frames that say nothing about the GPU */
        if (app.map && app.map.inflight) { frameGaps.length = 0; return; }
        frameGaps.push(gap);
        if (frameGaps.length < (frameGaps.length >= 5 && [...frameGaps].sort((a, b) => a - b)[2] > 200 ? 5 : 20)) return;
        const med = [...frameGaps].sort((a, b) => a - b)[frameGaps.length >> 1];
        frameGaps.length = 0;
        const pr = renderer.getPixelRatio();
        if (ts - lastPrChange < 1500) return;
        if (app.map && ((med > 40 && pr <= PR_MIN && app.quadBudget > app.BUDGET_MIN) || (med < 22 && app.quadBudget < app.BUDGET_MAX))) {
            const b = app.quadBudget;
            /* down from what is shown, by as much as the frames are over 30 ms */
            app.quadBudget = med > 40 ? Math.min(app.quadBudget, app.map.shownQuads) * THREE.MathUtils.clamp(30 / med, 0.3, 0.75) : app.quadBudget * 1.15;
            app.quadBudget = THREE.MathUtils.clamp(app.quadBudget, app.BUDGET_MIN, app.BUDGET_MAX);
            lastPrChange = ts;
            diag(`quad budget ${(b / 1e6).toFixed(2)} -> ${(app.quadBudget / 1e6).toFixed(2)} M (frames ${med.toFixed(0)} ms apart)`);
            return;
        }
        const next = med > 40 ? Math.max(PR_MIN, pr - 0.25) : med < 22 ? Math.min(PR_MAX, pr + 0.25) : pr;
        if (next === pr) return;
        lastPrChange = ts;
        renderer.setPixelRatio(next);
        sizeEffects();
        diag(`pixel ratio ${pr} -> ${next} (frames ${med.toFixed(0)} ms apart)`);
    }
    const eyeLocal = [0, 0, 0];
    function faceCullAll(dir) {
        for (const [o] of shadowMeshes) {
            if (!o.userData.face || !o.visible) continue;
            if (!dir) {
                const g = o.parent.position, s = 1 / o.parent.scale.x;
                eyeLocal[0] = (camera.position.x - g.x) * s; eyeLocal[1] = (camera.position.y - g.y) * s; eyeLocal[2] = (camera.position.z - g.z) * s;
            }
            app.faceCull(o, eyeLocal, dir && o.material.name === "OPAQUE" ? dir : null);
        }
    }
    let waveTimer = 0;
    /* The view (cam, at, sun, fly and the camera panel's settings) as
     * parameters: handed to onView a little after it stops changing (the
     * page keeps it in its address, so a reload comes back to it). */
    let viewTimer = 0, viewSaved = "";
    const r = v => v.toArray().map(x => Math.round(x * 10) / 10).join(",");
    function view() {
        const p = new URLSearchParams();
        p.set("cam", r(camera.position));
        p.set("at", r(controls.target));
        p.set("sun", String(Math.round(app.sunDeg * 10) / 10));
        if (app.flying) p.set("fly", "1");
        app.camParams(p);
        return p;
    }
    function saveView() {
        viewTimer = 0;
        const key = [r(camera.position), r(controls.target), app.sunDeg.toFixed(1), app.flying, app.camKey()].join(";");
        if (key === viewSaved || app.dead) return;
        viewSaved = key;
        if (app.onView) app.onView(view());
    }
    function draw() {
        if (app.dead) return;
        stats.draws++;
        if (!viewTimer) viewTimer = setTimeout(saveView, 500);
        waveU.uTime.value = (app.shot ? app.shot.time : performance.now()) / 1000;
        rippleU.uPixK.value = 2 * Math.tan(THREE.MathUtils.degToRad(camera.fov) / 2) / renderer.domElement.height / (app.shot ? app.shot.k : 1);
        sky.material.uniforms.uTime.value = waveU.uTime.value;
        if (app.map) app.updateTiles();
        if (!app.shot) renderShadow(); /* its fit follows the frustum: a photo's tiles keep the whole view's */
        faceCullAll(null);
        if (app.fx) fitTilt();
        if (app.shot) { tiltH.uniforms.uMax.value *= app.shot.k; tiltV.uniforms.uMax.value *= app.shot.k; }
        for (const m of waterMats) m.blendDstAlpha = app.fx ? THREE.ZeroFactor : THREE.OneFactor;
        linBlend.value = app.fx ? 1 : 0;
        rayU.uRayHere.value = app.fx ? 0 : 1;
        if (app.fx) composer.render(); else renderer.render(scene, camera);
        /* plants or rippling water in view: the wind keeps them moving; or the eye's shade still
           easing (see renderEyeShade), which needs no more frames than they do */
        if (((app.map ? app.map.moving : WAVE > 0 || RIPPLE > 0) || performance.now() < app.eyeSettle) && !waveTimer && !document.hidden && app.visible)
            waveTimer = setTimeout(() => { waveTimer = 0; requestDraw(); }, 1000 / WAVE_FPS);
    }
    app.on(document, "visibilitychange", () => { if (!document.hidden) requestDraw(); });

    Object.assign(app, { draw, requestDraw, timeFrame, view });
}
