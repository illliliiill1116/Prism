/*
 * The camera panel (C): focal length, exposure, white balance, aperture.
 */
import * as THREE from "three";

export function setupLens(app) {
    const { FOV_STD, camera, cvs, dofPass, fitZoomK, grade, hint, q, requestDraw } = app;
    /* ---- camera panel ----
     * A full-frame camera: focal length (the view's angle over a 24 mm high
     * frame), exposure compensation and white balance (a gain on linear light
     * in the grade), and the aperture, whose thin-lens depth of field is
     * DofPass, focused where the AF point is (the centre, or double-click).
     * A block is a metre, so as with a real lens the blur shows mostly with a
     * long lens or a near subject; "miniature" makes it MINI_SCALE times
     * smaller, a model landscape, and the blur that much wider (tilt-shift
     * photos look like models for this reason).
     * All but the focal length need FX. The settings go in the URL as ?fov=,
     * ?ev=, ?wb= and ?fstop=. Neither keeps the page drawing: each change is
     * one redraw. */
    const F_MIN = 12, F_MAX = 400, STOPS = [1, 1.4, 2, 2.8, 4, 5.6, 8, 11, 16, 22], MINI_SCALE = 20;
    const cam = {
        ev: q.has("ev") ? Number(q.get("ev")) : 0,
        wb: q.has("wb") ? Number(q.get("wb")) : 6500,
        stop: q.has("fstop") ? Math.max(0, STOPS.indexOf(Number(q.get("fstop")))) : STOPS.length,
        mini: q.get("mini") === "1",
    };
    if (q.has("fstop") && !STOPS.includes(Number(q.get("fstop")))) cam.stop = STOPS.length;
    const camEl = app.el;
    const camp = camEl("camp"), camBtn = camEl("camb");
    const czBar = camEl("cz"), cevBar = camEl("cev"), cwbBar = camEl("cwb"), cfBar = camEl("cf");
    const focal = () => 12 / Math.tan(THREE.MathUtils.degToRad(camera.fov) / 2);
    /* a black body's colour at k kelvin (Tanner Helland's fit), linear */
    function kelvin(k) {
        const t = k / 100, c = v => Math.pow(Math.min(255, Math.max(0, v)) / 255, 2.2);
        return t <= 66 ? [1, c(99.4708025861 * Math.log(t) - 161.1195681661), t <= 19 ? 0 : c(138.5177312231 * Math.log(t - 10) - 305.0447927307)]
                       : [c(329.698727446 * Math.pow(t - 60, -0.1332047592)), c(288.1221695283 * Math.pow(t - 60, -0.0755148492)), 1];
    }
    const WB_REF = kelvin(6500);
    function applyCam() {
        /* white balance: light of WB_REF's colour comes out as light of the setting's */
        const k = kelvin(cam.wb), g = WB_REF.map((v, i) => v / Math.max(k[i], 1e-3));
        const y = 0.2126 * g[0] + 0.7152 * g[1] + 0.0722 * g[2], e = Math.pow(2, cam.ev) / y;
        grade.uniforms.uGain.value.set(g[0] * e, g[1] * e, g[2] * e);
        const on = cam.stop < STOPS.length;
        dofPass.enabled = on;
        if (on) {
            const f = focal() / 1000;
            dofPass.zMat.uniforms.uK.value = 0.5 * f * f / STOPS[cam.stop] / 0.024 * app.height * (cam.mini ? MINI_SCALE : 1);
        }
        camEl("czv").value = `${Math.round(focal())} mm`;
        camEl("cevv").value = `${cam.ev > 0 ? "+" : cam.ev < 0 ? "−" : "±"}${Math.abs(cam.ev).toFixed(1)} EV`;
        camEl("cwbv").value = `${cam.wb} K`;
        camEl("cfv").value = on ? `f/${STOPS[cam.stop]}` : "pinhole";
        camEl("cmini").classList.toggle("on", cam.mini);
        camEl("cnote").textContent = app.fx ? (on ? "double-click to focus there" : "aperture: open it for depth of field")
                                        : "exposure, white balance and aperture need FX (F)";
    }
    function setFocal(f) {
        f = Math.min(F_MAX, Math.max(F_MIN, f));
        camera.fov = THREE.MathUtils.radToDeg(2 * Math.atan(12 / f));
        camera.updateProjectionMatrix();
        fitZoomK();
        czBar.value = String(Math.round(1000 * Math.log(f / F_MIN) / Math.log(F_MAX / F_MIN)));
        applyCam();
        requestDraw();
    }
    function syncCamBars() {
        czBar.value = String(Math.round(1000 * Math.log(focal() / F_MIN) / Math.log(F_MAX / F_MIN)));
        cevBar.value = String(cam.ev); cwbBar.value = String(cam.wb); cfBar.value = String(cam.stop);
    }
    czBar.addEventListener("input", () => setFocal(F_MIN * Math.exp(czBar.value / 1000 * Math.log(F_MAX / F_MIN))));
    cevBar.addEventListener("input", () => { cam.ev = Number(cevBar.value); applyCam(); requestDraw(); });
    cwbBar.addEventListener("input", () => { cam.wb = Number(cwbBar.value); applyCam(); requestDraw(); });
    cfBar.addEventListener("input", () => { cam.stop = Number(cfBar.value); applyCam(); requestDraw(); });
    /* let go of a bar, so the arrow keys move the camera again */
    for (const b of [czBar, cevBar, cwbBar, cfBar]) b.addEventListener("change", () => b.blur());
    function camKey() { return [camera.fov.toFixed(2), cam.ev, cam.wb, cam.stop, cam.mini].join(","); }
    function camParams(p) {
        if (Math.abs(camera.fov - FOV_STD) > 0.01) p.set("fov", camera.fov.toFixed(2)); else p.delete("fov");
        if (cam.ev) p.set("ev", String(cam.ev)); else p.delete("ev");
        if (cam.wb !== 6500) p.set("wb", String(cam.wb)); else p.delete("wb");
        if (cam.stop < STOPS.length) p.set("fstop", String(STOPS[cam.stop])); else p.delete("fstop");
        if (cam.mini) p.set("mini", "1"); else p.delete("mini");
    }
    function setFocus(x, y) {
        dofPass.zMat.uniforms.uFocusUv.value.set(x, y);
        camEl("caf").classList.toggle("on", x !== 0.5 || y !== 0.5);
        requestDraw();
    }
    camEl("caf").addEventListener("click", () => setFocus(0.5, 0.5));
    camEl("cmini").addEventListener("click", () => {
        cam.mini = !cam.mini;
        if (cam.mini && cam.stop >= STOPS.length) { cam.stop = STOPS.indexOf(2.8); syncCamBars(); }
        applyCam();
        requestDraw();
    });
    cvs.addEventListener("dblclick", e => {
        if (!dofPass.enabled || document.pointerLockElement === cvs) return;
        const b = cvs.getBoundingClientRect();
        setFocus((e.clientX - b.left) / b.width, 1 - (e.clientY - b.top) / b.height);
        hint("focus set: AF centre to go back");
    });
    camEl("creset").addEventListener("click", () => {
        Object.assign(cam, { ev: 0, wb: 6500, stop: STOPS.length, mini: false });
        setFocus(0.5, 0.5);
        setFocal(12 / Math.tan(THREE.MathUtils.degToRad(FOV_STD) / 2));
        syncCamBars();
    });
    const setClean = on => { app.root.classList.toggle("clean", on); if (on) hint("H shows the controls again"); };
    camEl("cshot").addEventListener("click", () => app.shoot());
    camEl("cclean").addEventListener("click", () => setClean(true));
    const setCamPanel = on => { camp.hidden = !on; camBtn.classList.toggle("on", on); if (on) applyCam(); };
    camBtn.addEventListener("click", () => { camBtn.blur(); setCamPanel(camp.hidden); });
    app.on(app.keys, "keydown", e => {
        if (e.ctrlKey || e.metaKey || e.altKey || e.target instanceof HTMLInputElement && e.target.type !== "range") return;
        if (e.code === "KeyC") setCamPanel(camp.hidden);
        else if (e.code === "KeyH") setClean(!app.root.classList.contains("clean"));
        else if (e.code === "KeyP") app.shoot();
    });
    syncCamBars();
    applyCam();

    Object.assign(app, { applyCam, cam, camEl, camKey, camParams, setFocal, setFocus });
}
