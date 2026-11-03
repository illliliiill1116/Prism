/*
 * prism's viewer (three.js): a baked world's tiles at the level of detail
 * their distance calls for, lit for wherever the sun is.
 *
 *     import { createViewer } from "./js/viewer.js";
 *     const viewer = createViewer(element, { map: "maps/world", sun: 120 });
 *
 * puts one in an element of the page, filling it and following its size.
 * The options are the URL parameters the README lists (index.html, a page
 * that is a viewer and nothing else, passes its own address on), plus:
 *
 *   keys     "focus" (default): the keys work while the viewer has focus
 *            (a click or a tab into it); "page": anywhere on the page
 *   ui       false: start with the controls hidden (H shows them)
 *   onView   (params: URLSearchParams) => {}, a little after the view
 *            stops changing: cam, at, sun, fly and the camera panel's
 *   onReady  () => {}, once everything in view has loaded
 *   onPhoto  (canvas) => {}, in place of downloading a photo
 *
 * Each viewer is one WebGL context; destroy() frees it. The modules each
 * add their part to one object, app, shared by all of them (setupX(app)),
 * in the order below: a module may use what those before it added as soon
 * as it is set up, and what those after it add only once they are.
 */
import * as THREE from "three";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";
import { setupDiag, setupBench } from "./diag.js";
import { setupSun } from "./sun.js";
import { setupAir } from "./air.js";
import { setupSky } from "./sky.js";
import { setupShadows } from "./shadows.js";
import { setupMaterials } from "./materials.js";
import { setupEffects } from "./effects.js";
import { setupFrame } from "./frame.js";
import { setupMap } from "./map.js";
import { setupGlb } from "./glb.js";
import { setupControls } from "./controls.js";
import { setupLens } from "./lens.js";
import { setupPhoto } from "./photo.js";

/* the URL parameters that make up the view (see onView) */
export const VIEW_KEYS = ["cam", "at", "sun", "fly", "fov", "ev", "wb", "fstop", "mini"];

const FULL_ICON = `<svg viewBox="0 0 20 20" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"
  stroke-linecap="round" aria-hidden="true"><path d="M3 7V3h4M13 3h4v4M17 13v4h-4M7 17H3v-4"/></svg>`;
/* a person, arms out: fly mode */
const FLY_ICON = `<svg viewBox="0 0 20 20" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"
  stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="10" cy="4" r="2"/><path
  d="M3 9l7-1 7 1M10 8v4M10 12l-3 6M10 12l3 6"/></svg>`;
export const PLAY_ICON = `<svg viewBox="0 0 20 20" width="16" height="16" aria-hidden="true"><path d="M6 3.5l10 6.5-10 6.5z" fill="currentColor"/></svg>`;
export const PAUSE_ICON = `<svg viewBox="0 0 20 20" width="16" height="16" aria-hidden="true"><path d="M5 4h3.5v12H5zM11.5 4H15v12h-3.5z" fill="currentColor"/></svg>`;
const UI = `
<div class="prism-info"></div>
<div class="prism-sunp prism-panel"><button class="prism-sunrun" title="let the day go by (T)">${PLAY_ICON}</button><input
  class="prism-sunbar" type="range" min="0" max="1440" step="5" aria-label="time of day" title="time of day ([ and ])"><output class="prism-sunt"></output></div>
<div class="prism-pad prism-panel">
  <button data-k="d" title="down (Q)">Q</button><button data-k="f" title="forward (W)">W</button><button data-k="u" title="up (E)">E</button>
  <button data-k="l" title="left (A)">A</button><button data-k="b" title="back (S)">S</button><button data-k="r" title="right (D)">D</button>
</div>
<div class="prism-zoom prism-panel">
  <button class="prism-zin" title="zoom in">+</button>
  <input class="prism-zbar" type="range" min="0" max="1000" aria-label="zoom">
  <button class="prism-zout" title="zoom out">−</button>
</div>
<div class="prism-fxp prism-panel"><button class="prism-fly" title="fly (V)">${FLY_ICON}</button><button
  class="prism-camb" title="camera (C)">CAM</button><button class="prism-full" title="full screen">${FULL_ICON}</button><button
  class="prism-helpb" title="help (?)">?</button></div>
<div class="prism-help prism-panel" hidden><button class="prism-helpx" title="close (Esc)">×</button>
  <dl>
    <dt>Look around</dt><dd>drag; right-drag pans, the wheel or the bar on the right zooms</dd>
    <dt>Move</dt><dd><kbd>W</kbd><kbd>A</kbd><kbd>S</kbd><kbd>D</kbd> or the pad, <kbd>Q</kbd>/<kbd>E</kbd> down/up, <kbd>Shift</kbd> faster</dd>
    <dt>Fly</dt><dd><kbd>V</kbd> or the person button; click to steer with the mouse, <kbd>Esc</kbd> lets go, the wheel sets the speed</dd>
    <dt>Time of day</dt><dd>the slider top left, or <kbd>[</kbd> <kbd>]</kbd>; <kbd>T</kbd> or ▶ lets the day go by</dd>
    <dt>Camera</dt><dd><kbd>C</kbd> or CAM: focal length, exposure, white balance, aperture</dd>
    <dt>More</dt><dd><kbd>P</kbd> photo, <kbd>F</kbd> effects on/off, <kbd>H</kbd> hide the controls, <kbd>?</kbd> this help</dd>
  </dl>
</div>
<div class="prism-camp prism-panel" hidden>
  <label>focal</label><input class="prism-cz" type="range" min="0" max="1000" aria-label="focal length"><output class="prism-czv"></output>
  <label>exposure</label><input class="prism-cev" type="range" min="-3" max="3" step="0.1" aria-label="exposure"><output class="prism-cevv"></output>
  <label>white bal.</label><input class="prism-cwb" type="range" min="2500" max="10000" step="100" aria-label="white balance"><output class="prism-cwbv"></output>
  <label>aperture</label><input class="prism-cf" type="range" min="0" max="10" step="1" aria-label="aperture"><output class="prism-cfv"></output>
  <div class="prism-note prism-cnote"></div>
  <div class="prism-row"><button class="prism-caf" title="focus on the centre">AF centre</button><button class="prism-cmini" title="depth of field as if a block were 5 cm: a model landscape">miniature</button></div>
  <div class="prism-row"><button class="prism-creset">reset</button><button class="prism-cshot" title="save a PNG (P)">photo</button><select class="prism-cscale" title="photo size: times the screen's">
    <option value="1">1×</option><option value="2" selected>2×</option><option value="3">3×</option><option value="4">4×</option></select><button class="prism-cclean" title="hide the controls (H)">hide UI</button></div>
</div>
<div class="prism-hint"></div>`;

/* Options as URL parameters read: strings, true / false as "1" / "0", a
   list (cam: [x, y, z]) joined with commas; callbacks and the like left out. */
function params(options) {
    const o = new Map();
    for (const [k, v] of Object.entries(options)) {
        if (v === undefined || v === null || typeof v === "function") continue;
        if (Array.isArray(v)) o.set(k, v.join(","));
        else if (typeof v === "boolean") o.set(k, v ? "1" : "0");
        else if (typeof v !== "object") o.set(k, String(v));
    }
    return { has: k => o.has(k), get: k => (o.has(k) ? o.get(k) : null) };
}

/* the stylesheet, next to js/, unless the page has it */
function addStyle() {
    if (document.querySelector("link[data-prism], style[data-prism]")) return;
    const l = Object.assign(document.createElement("link"), { rel: "stylesheet", href: new URL("../style.css", import.meta.url).href });
    l.dataset.prism = "";
    document.head.appendChild(l);
}

export function createViewer(container, options = {}) {
    addStyle();
    const q = params(options);
    const root = document.createElement("div");
    root.className = "prism";
    root.tabIndex = 0;
    root.innerHTML = UI;
    if (options.ui === false) root.classList.add("clean");
    container.appendChild(root);
    const listeners = [];
    const app = {
        q, root,
        el: name => root.querySelector(".prism-" + name),
        width: Math.max(1, root.clientWidth), height: Math.max(1, root.clientHeight),
        keys: options.keys === "page" ? window : root,
        onView: options.onView, onPhoto: options.onPhoto,
        dead: false, visible: true, shot: null, map: null,
        timers: new Set(),
        /* a listener on the page, taken off again by destroy */
        on(target, type, fn, opts) { target.addEventListener(type, fn, opts); listeners.push([target, type, fn, opts]); },
    };
    app.info = app.el("info");
    let ready = false;
    Object.defineProperty(app, "ready", {
        get: () => ready,
        set: v => { const was = ready; ready = v; if (v === true && was !== true && options.onReady) options.onReady(); },
    });

    const renderer = new THREE.WebGLRenderer({ antialias: true, preserveDrawingBuffer: true });
    /* Pixel ratio: at most 2 (a phone's 3 costs 1.6x the frame for little to
       see), lowered while frames come slow (see adaptResolution); ?pr= fixes it. */
    const PR_MAX = q.has("pr") ? Number(q.get("pr")) : Math.min(window.devicePixelRatio, 2);
    const PR_MIN = q.has("pr") ? PR_MAX : Math.min(1, PR_MAX);
    renderer.setPixelRatio(PR_MAX);
    renderer.setSize(app.width, app.height);
    root.prepend(renderer.domElement);
    Object.assign(app, { renderer, PR_MAX, PR_MIN });
    setupDiag(app);

    const scene = new THREE.Scene();
    const camera = new THREE.PerspectiveCamera(Number(q.get("fov")) || 50, app.width / app.height, 0.5, 3000);
    /* how much nearer the lens brings things than the standard 50 degree view:
       level of detail and plant fade go by distance times this, so a long lens
       loads the detail it can see */
    const FOV_STD = 50;
    const zoomK = { value: 1 };
    const fitZoomK = () => { zoomK.value = Math.tan(THREE.MathUtils.degToRad(camera.fov) / 2) / Math.tan(THREE.MathUtils.degToRad(FOV_STD) / 2); };
    fitZoomK();
    const vec = (s, d) => (s ? s.split(",").map(Number) : d);
    camera.position.fromArray(vec(q.get("cam"), [-180, 200, 220]));
    const controls = new OrbitControls(camera, renderer.domElement);
    controls.target.fromArray(vec(q.get("at"), [0, 64, 0]));
    controls.update();
    camera.layers.enableAll(); /* layer 1 too: see SplitRenderPass */
    const MSAA = renderer.getContext().getParameter(renderer.getContext().SAMPLES) > 0;
    const mapDir = q.get("map");
    Object.assign(app, { scene, camera, controls, FOV_STD, zoomK, fitZoomK, MSAA, mapDir, t0: performance.now() });

    setupSun(app);
    setupAir(app);
    setupSky(app);
    app.placeSun(app.sunDeg);
    app.fitFog();
    setupShadows(app);
    setupMaterials(app);
    setupEffects(app);
    setupFrame(app);
    setupMap(app);
    setupGlb(app);
    setupControls(app);
    setupLens(app);
    setupPhoto(app);
    setupBench(app);

    /* the element's size, whatever changes it */
    function resize() {
        const w = root.clientWidth, h = root.clientHeight;
        if (!w || !h || (w === app.width && h === app.height)) return;
        app.width = w; app.height = h;
        camera.aspect = w / h;
        camera.updateProjectionMatrix();
        renderer.setSize(w, h);
        app.sizeEffects();
        app.applyCam();
        app.draw();
    }
    const sizes = new ResizeObserver(resize);
    sizes.observe(root);
    /* scrolled out of sight, the wind and water stop asking for frames */
    const sight = new IntersectionObserver(es => {
        app.visible = es[es.length - 1].isIntersecting;
        if (app.visible) app.requestDraw();
    });
    sight.observe(root);
    /* keys go to the viewer once it is clicked */
    root.addEventListener("pointerdown", () => { if (document.activeElement !== root && !root.contains(document.activeElement)) root.focus({ preventScroll: true }); });

    /* Full screen: the element itself, or where a browser cannot (an
       iPhone's), the window filled with it (max) until Esc or the button. */
    const fullEl = () => document.fullscreenElement || document.webkitFullscreenElement;
    const isFull = () => fullEl() === root || root.classList.contains("max");
    const fullBtn = app.el("full");
    function setFullscreen(on = !isFull()) {
        if (on === isFull()) return;
        const req = root.requestFullscreen || root.webkitRequestFullscreen;
        const fill = () => { root.classList.toggle("max", on); syncFull(); };
        if (root.classList.contains("max") || !req || !(document.fullscreenEnabled || document.webkitFullscreenEnabled)) fill();
        else if (on) Promise.resolve(req.call(root)).catch(fill);
        else (document.exitFullscreen || document.webkitExitFullscreen).call(document);
    }
    function syncFull() {
        fullBtn.classList.toggle("on", isFull());
        fullBtn.title = isFull() ? "leave full screen (Esc)" : "full screen";
    }
    fullBtn.addEventListener("click", () => { fullBtn.blur(); setFullscreen(); });
    app.on(document, "fullscreenchange", syncFull);
    app.on(document, "webkitfullscreenchange", syncFull);
    app.on(app.keys, "keydown", e => { if (e.code === "Escape" && root.classList.contains("max")) setFullscreen(false); });

    function destroy() {
        if (app.dead) return;
        app.dead = true;
        if (fullEl() === root) (document.exitFullscreen || document.webkitExitFullscreen).call(document);
        if (document.pointerLockElement === renderer.domElement) document.exitPointerLock();
        for (const [t, type, fn, opts] of listeners) t.removeEventListener(type, fn, opts);
        for (const id of app.timers) clearInterval(id);
        sizes.disconnect();
        sight.disconnect();
        app.stopTiles();
        controls.dispose();
        renderer.dispose();
        renderer.forceContextLoss();
        root.remove();
    }

    const mapPath = mapDir && mapDir.replace(/\/$/, "");
    if (mapPath) app.loadMap(mapPath); else app.loadGlb(q.get("glb") || "region.glb");

    /* for headless tests: the viewer's insides as globals, and photos to __shotSink */
    if (options.testHooks) {
        Object.assign(window, {
            __camera: camera, __scene: scene, __controls: controls, __renderer: renderer, __viewer: app,
            __shaftPass: app.shaftPass, __shadowBox: app.shadowBox, __stats: app.stats,
            __drawOnly: () => app.draw(), /* the CPU side of a frame */
            __timeFrame: app.timeFrame, __eyeShade: app.eyeShade, __setSun: app.setSun, __setFly: app.setFly, __shoot: app.shoot,
            __cam: { cam: app.cam, setFocal: app.setFocal, setFocus: app.setFocus, applyCam: app.applyCam, dofPass: app.dofPass },
        });
        Object.defineProperty(window, "__ready", { get: () => app.ready, configurable: true });
        Object.defineProperty(window, "__map", { get: () => app.map, configurable: true });
        if (!options.onPhoto) Object.defineProperty(app, "onPhoto", { get: () => window.__shotSink });
    }

    return {
        element: root,
        get ready() { return app.ready === true; },
        /* the view as onView has it, as an object of strings */
        view: () => Object.fromEntries(app.view()),
        /* cam and at: [x, y, z] in world coordinates */
        setView({ cam, at }) {
            if (cam) camera.position.fromArray(cam);
            if (at) controls.target.fromArray(at);
            controls.update();
            if (app.flying) app.syncLook();
            app.fitFog();
            app.syncBar();
            app.requestDraw();
        },
        /* deg: 0 sunrise (east), 180 sunset, 180-360 night */
        setSun(deg) { app.setSun(deg); app.requestDraw(); },
        setFly: on => app.setFly(!!on),
        setFx: on => app.setFx(!!on),
        setFullscreen,
        /* a PNG of the view, scale times its size a side (1-4) */
        photo: scale => app.shoot(scale),
        destroy,
        three: { THREE, renderer, scene, camera, controls },
    };
}
