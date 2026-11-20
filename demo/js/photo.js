/*
 * Photos (P): the view saved as a PNG, up to 4x the screen's size.
 */
import * as THREE from "three";

export function setupPhoto(app) {
    const { DOF_MAX, applyCam, bloom, bloomView, camEl, camera, controls, dofPass, draw, fitZoomK, hint, renderer,
            shotBloom, shotFrame, shotGrab, tiltH, tiltV, waterU, zoomK } = app;
    /* A photo k times the screen's size each way, drawn in tiles of the
     * screen's size through a view offset, each with a margin of SHOT_PAD that
     * is cut off, so what reaches over a tile's edge (blur, bloom, water's
     * screen-space reflections fading out at the edge) is drawn as in one
     * frame. First the detail a screen k times as sharp would load comes in
     * (up to SHOT_WAIT ms). In the tiles the shadow maps, the eye's shade and
     * the time stand still, the effects measured in pixels grow k times, the
     * vignette and tilt band follow the whole picture (shotFrame), and the
     * depth of field takes the focus depth from the whole view, the water's
     * reflections the whole view's scene (shotGrab), the bloom the whole
     * view's (shotBloom). */
    const SHOT_PAD = 0.12, SHOT_WAIT = 20000, SHOT_MAX_PX = 200e6, SHOT_MAX_SIDE = 16384;
    const scaleSel = camEl("cscale");
    scaleSel.addEventListener("change", () => scaleSel.blur());
    const frameNext = () => new Promise(r => setTimeout(r, 0));
    function save(cv, k) {
        if (app.onPhoto) { app.onPhoto(cv); return; }
        cv.toBlob(b => {
            const a = document.createElement("a"), d = new Date(), z = n => String(n).padStart(2, "0");
            a.href = URL.createObjectURL(b);
            a.download = `prism-${d.getFullYear()}${z(d.getMonth() + 1)}${z(d.getDate())}-${z(d.getHours())}${z(d.getMinutes())}${z(d.getSeconds())}${k > 1 ? `-${cv.width}x${cv.height}` : ""}.png`;
            a.click();
            setTimeout(() => URL.revokeObjectURL(a.href), 1000);
            hint(`photo saved, ${cv.width}×${cv.height}`);
        }, "image/png");
    }
    let shooting = false;
    /* scale: the photo's size in screens a side, else the panel's */
    async function shoot(scale) {
        if (shooting) return;
        const W = renderer.domElement.width, H = renderer.domElement.height;
        let k = Number(scale) || Number(scaleSel.value) || 1;
        while (k > 1 && (W * k * H * k > SHOT_MAX_PX || Math.max(W, H) * k > SHOT_MAX_SIDE)) k--;
        if (k === 1) { draw(); save(renderer.domElement, 1); return; } /* preserveDrawingBuffer keeps it */
        shooting = true;
        controls.enabled = false;
        try {
            /* the detail for the larger picture */
            if (app.map) {
                zoomK.value /= k;
                const t0 = performance.now();
                for (let n = 0; performance.now() - t0 < SHOT_WAIT; ) {
                    hint(`photo ${k}×: loading detail…`, 60000);
                    draw();
                    await new Promise(r => setTimeout(r, 100));
                    n = app.map.settled ? n + 1 : 0;
                    if (n >= 3) break;
                }
            }
            shotGrab.want = true;
            draw();
            shotGrab.want = false;
            if (bloom.enabled && app.fx) { shotBloom.keep(renderer); bloom.enabled = false; bloomView.enabled = true; }
            waterU.uFullOn.value = 1;
            /* the focus depth, from the whole view's blur radii target (g: depth) */
            const dofU = dofPass.zMat.uniforms, blurU = dofPass.blur.uniforms;
            if (dofPass.enabled) {
                const px = new Uint16Array(4), z = dofPass.z, f = dofU.uFocusUv.value;
                renderer.readRenderTargetPixels(z, Math.min(z.width - 1, Math.floor(f.x * z.width)), Math.min(z.height - 1, Math.floor(f.y * z.height)), 1, 1, px);
                dofU.uFocusZ.value = THREE.DataUtils.fromHalfFloat(px[1]);
            }
            dofU.uK.value *= k; dofU.uMaxR.value = blurU.uMaxR.value = DOF_MAX * k; blurU.uTaps.value = 10 * k;
            tiltH.uniforms.uTaps.value = tiltV.uniforms.uTaps.value = 7 * k;
            const FW = W * k, FH = H * k, px = Math.round(W * SHOT_PAD), py = Math.round(H * SHOT_PAD);
            const cw = W - 2 * px, ch = H - 2 * py, nx = Math.ceil(FW / cw), ny = Math.ceil(FH / ch);
            const out = document.createElement("canvas");
            out.width = FW; out.height = FH;
            const ctx = out.getContext("2d");
            app.shot = { k, time: performance.now() };
            for (let j = 0; j < ny; j++) for (let i = 0; i < nx; i++) {
                hint(`photo ${k}×: tile ${j * nx + i + 1} of ${nx * ny}`, 60000);
                await frameNext();
                const x = i * cw, y = j * ch;
                camera.setViewOffset(FW, FH, x - px, y - py, W, H);
                shotFrame.value.set((x - px) / FW, 1 - (y - py + H) / FH, 1 / k, 1 / k);
                draw();
                ctx.drawImage(renderer.domElement, px, py, Math.min(cw, FW - x), Math.min(ch, FH - y), x, y, Math.min(cw, FW - x), Math.min(ch, FH - y));
            }
            hint(`photo ${k}×: saving ${FW}×${FH}…`, 60000);
            await frameNext();
            save(out, k);
        } catch (e) {
            console.warn(e);
            hint(`photo failed: ${e.message || e}`);
        } finally {
            app.shot = null;
            shotGrab.want = false;
            waterU.uFullOn.value = 0;
            shotGrab.free();
            if (bloomView.enabled) { bloomView.enabled = false; bloom.enabled = true; shotBloom.free(); }
            camera.clearViewOffset();
            shotFrame.value.set(0, 0, 1, 1);
            dofPass.zMat.uniforms.uFocusZ.value = 0;
            dofPass.zMat.uniforms.uMaxR.value = dofPass.blur.uniforms.uMaxR.value = DOF_MAX;
            dofPass.blur.uniforms.uTaps.value = 10;
            tiltH.uniforms.uTaps.value = tiltV.uniforms.uTaps.value = 7;
            fitZoomK();
            applyCam();
            controls.enabled = true;
            shooting = false;
            draw();
        }
    }

    Object.assign(app, { shoot });
}
