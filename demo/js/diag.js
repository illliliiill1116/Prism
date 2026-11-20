/*
 * ?diag: what the page sees and what goes wrong, on screen and in the
 * server's log (as requests for __diag?<message>, which 404), for devices
 * without a console at hand; ?diag&bench times frames once the tiles are in.
 */
import * as THREE from "three";

export function setupDiag(app) {
    const { q, renderer } = app;
    const DIAG = q.has("diag");
    const diagEl = DIAG ? app.root.appendChild(Object.assign(document.createElement("pre"), { className: "prism-diag" })) : null;
    const diagLines = [];
    function diag(msg) {
        if (!DIAG) return;
        diagLines.push(msg);
        diagEl.textContent = diagLines.slice(-14).join("\n");
        fetch("__diag?" + encodeURIComponent(msg.slice(0, 1500))).catch(() => {});
    }
    if (DIAG) {
        Object.assign(diagEl.style, { position: "absolute", left: "8px", bottom: "8px", right: "8px", margin: 0, maxHeight: "45%", overflow: "auto",
            font: "11px/1.3 monospace", color: "#fff", background: "rgba(0,0,0,0.6)", padding: "6px", whiteSpace: "pre-wrap", zIndex: 10, pointerEvents: "none" });
        app.on(window, "error", e => diag(`error: ${e.message} @${e.filename}:${e.lineno}`));
        app.on(window, "unhandledrejection", e => diag(`rejection: ${e.reason && (e.reason.stack || e.reason)}`));
        const gl = renderer.getContext(), dbg = gl.getExtension("WEBGL_debug_renderer_info");
        diag(`${navigator.userAgent}`);
        diag(`gpu ${dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER)}; dpr ${devicePixelRatio}; ` +
             `${app.width}x${app.height}; max tex ${gl.getParameter(gl.MAX_TEXTURE_SIZE)}; mem ${navigator.deviceMemory || "?"} GB; ` +
             `half float rt ${!!gl.getExtension("EXT_color_buffer_half_float") || !!gl.getExtension("EXT_color_buffer_float")}`);
        renderer.domElement.addEventListener("webglcontextlost", () => diag("WEBGL CONTEXT LOST"));
        renderer.debug.onShaderError = (gl2, program, vs, fs) => {
            const log = [gl2.getProgramInfoLog(program), gl2.getShaderInfoLog(vs), gl2.getShaderInfoLog(fs)].filter(Boolean).join(" | ");
            diag("SHADER ERROR: " + log.slice(0, 1200));
        };
    }
    Object.assign(app, { DIAG, diag });
}

export function setupBench(app) {
    const { DIAG, camera, controls, diag, draw, info, q, renderShadow, renderer, shadowBox, sizeEffects, stats } = app;
    /* ?diag: every 5 s, a timed frame and what is held */
    if (DIAG) app.timers.add(setInterval(() => {
        const gl = renderer.getContext();
        if (gl.isContextLost()) { diag("context lost"); return; }
        const ms = app.timeFrame(false), m = renderer.info.memory;
        const heap = performance.memory ? ` heap ${(performance.memory.usedJSHeapSize / 1e6).toFixed(0)} MB;` : "";
        diag(`frame ${ms.toFixed(0)} ms; fx ${app.fx};${heap} geometries ${m.geometries}, textures ${m.textures}; ` +
             `${info.textContent}${app.map ? `; ${(app.map.wire / 1e6).toFixed(1)} MB down` : ""}`);
    }, 5000));
    /* ?diag&bench: once the tiles are in, the median of 5 timed frames per
       pixel ratio with and without FX, and of the shadow map pass alone */
    if (DIAG && q.has("bench")) {
        const wait = setInterval(async () => {
            if (app.ready !== true) return;
            clearInterval(wait);
            const gl = renderer.getContext();
            const sync = () => gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(4));
            const med = f => { const t = []; for (let i = 0; i < 5; i++) t.push(f()); return t.sort((x, y) => x - y)[2]; };
            const fx0 = app.fx, pr0 = renderer.getPixelRatio();
            diag(`bench shadow map pass ${med(() => { shadowBox.H = 0; const t = performance.now(); renderShadow(); sync(); return performance.now() - t; }).toFixed(0)} ms`);
            for (const pr of [...new Set([devicePixelRatio, 2, 1.5, 1, 0.75])].filter(p => p <= devicePixelRatio))
                for (const f of [true, false]) {
                    renderer.setPixelRatio(pr);
                    sizeEffects();
                    app.fx = f;
                    app.timeFrame(false);
                    diag(`bench pr ${pr} fx ${f}: ${med(() => app.timeFrame(false)).toFixed(0)} ms`);
                    await new Promise(r => setTimeout(r, 50));
                }
            renderer.setPixelRatio(pr0);
            sizeEffects();
            app.fx = fx0;
            /* an orbit, 2 degrees a frame, shadow map redraws included */
            const t = [], s0 = stats.shadows, off = camera.position.clone().sub(controls.target);
            for (let i = 0; i < 40; i++) {
                off.applyAxisAngle(new THREE.Vector3(0, 1, 0), Math.PI / 90);
                camera.position.copy(controls.target).add(off);
                controls.update();
                t.push(app.timeFrame(false));
            }
            t.sort((x, y) => x - y);
            diag(`bench orbit at pr ${pr0} fx ${fx0}: median ${t[20].toFixed(0)}, p90 ${t[36].toFixed(0)}, max ${t[39].toFixed(0)} ms; ${stats.shadows - s0} shadow redraws in 40`);
            draw();
            diag("bench done");
        }, 500);
    }
}
