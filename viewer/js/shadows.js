/*
 * The live shadow maps: the near sun shadows, and the light shafts' maps.
 *
 * Sun shadows from blockers within a few blocks (steps, fences, grass,
 * flowers) come from a live shadow map, hard edged (smoothed over about a
 * texel); the bake keeps the soft ones from trees and terrain.
 * Thin blocks (fences, posts: _SUN alpha THIN_A) are split the same way
 * further out (THIN_NEAR1), the bake taking their far shadows as whole
 * voxels. Leaves cast as whole blocks, as the
 * bake takes them.
 * Plants cast light shadows and take none from each other. Leaves take
 * none from the map: their shade is baked, light let through their outer
 * layers, and they are lit as foliage (LEAF_SIDE, ?leafside= ?leafback=
 * ?leaftint=); plants likewise (?plantsss=), and sway in the wind
 * (?wave=, 0 = still; frames keep coming while plants are in view).
 * ?shadow=0
 * turns the map off. The map is laid on the ground (an oblique projection,
 * so its texels stay put as the sun moves), and while the sun moves two
 * maps a SUN_STEP apart are blended rather than one redrawn every frame,
 * which made the hard edges shimmer.
 */
import * as THREE from "three";

export function setupShadows(app) {
    const { EYE_GLSL, RAYS, RAY_FAR, SUN_DIR, camera, controls, q, rayU, renderer, scene, sky, sunAt } = app;
    /* ---- near sun shadows ----
     * The bake leaves out blockers that start within NEAR0..NEAR1 blocks along
     * the sun ray; this map puts them back, hard edged, and catches what the
     * voxel bake cannot see at all (grass, flowers, fences). It holds what
     * faces the sun - the distance at which the ray leaves the topmost blocker,
     * as baked - and both sides of cut-out geometry. (Back faces would leave a
     * hole under every block standing on the ground, whose bottom face is
     * culled, and light a band along its foot.) It covers
     * the ground in view, up to 800 blocks across. Plants go in a map of their
     * own: in one map a grass blade on top of a wall hid the wall behind it and
     * punched light holes into the wall's shadow. They cast PLANT_CAST-dark
     * shadows, and take shade only from solid blockers.
     * Edges stay hard: the penumbra is about a texel, widening slightly with
     * the distance to the blocker (PENUMBRA blocks per block), enough to
     * smooth the texel steps. */
    const SHADOW_SIZE = 4096, NEAR0 = 4.0, NEAR1 = 6.0, SHADOW_RANGE = 2000;
    const PENUMBRA = 0.012, PLANT_CAST = 0.5, THIN_NEAR0 = 8.0, THIN_NEAR1 = 12.0;
    const PLANT_BIAS = 0.15; /* blocks: see nearShadowIn */
    const shadowOn = q.get("shadow") !== "0";
    function depthRT(size = SHADOW_SIZE) {
        const n = shadowOn ? size : 1;
        /* the depth is read, and the solid map's colour: the thin flag (see THIN_A) */
        const rt = new THREE.WebGLRenderTarget(n, n, { format: THREE.RedFormat, generateMipmaps: false });
        rt.depthTexture = new THREE.DepthTexture(n, n, THREE.UnsignedIntType);
        return rt;
    }
    /* Maps are drawn at whole SUN_STEPs of the sun's arc while it moves, and
     * the two either side of it blended: a map drawn afresh for every small
     * step resampled every edge and the hard shadows shimmered. Still, the
     * one map is drawn where the sun is. */
    const SUN_STEP = 1;
    /* [solid, plant, water] maps; the water's (where the sun comes into it,
       for CAUSTIC_GLSL) needs no fine detail */
    const WATER_MAP_SIZE = 1024;
    const slotRTs = () => [depthRT(), depthRT(), depthRT(WATER_MAP_SIZE)];
    const shadowSlots = [{ rts: slotRTs(), deg: NaN }, null]; /* the second made when first needed */
    /* The map looks along the sun but is laid out on the ground: a point is
     * pushed along the sun ray to the plane y = shadowY0 and its x, z there
     * index the map (an oblique projection), so its texels stay put in the
     * world as the sun moves rather than turning with it, and most edges land
     * on the same texels in the maps either side. Depth is the distance along
     * the ray, as before. All of it is in the camera's projection matrix (the
     * camera itself stays at the origin). */
    const lightCam = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, SHADOW_RANGE);
    lightCam.matrixWorldAutoUpdate = false;
    lightCam.layers.enableAll(); /* water too, for its map */
    const sunShear = new THREE.Vector2(); /* x, z moved per block up */
    let shadowY0 = 0;
    app.sunMoving = false;
    function shearOf(S, out) {
        const sy = Math.max(S.y, 0.08);
        return out.set(S.x / sy, S.z / sy);
    }
    function aimLight() { shearOf(SUN_DIR, sunShear); }
    /* where p lands on the map's plane, in out.x, out.y */
    function lightUV(p, out) {
        out.x = p.x - (p.y - shadowY0) * sunShear.x;
        out.y = p.z - (p.y - shadowY0) * sunShear.y;
        return out;
    }
    const shadowU = {
        tShadow: { value: shadowSlots[0].rts[0].depthTexture }, tPlant: { value: shadowSlots[0].rts[1].depthTexture },
        tThin: { value: shadowSlots[0].rts[0].texture }, tThin2: { value: shadowSlots[0].rts[0].texture },
        uShadowMat: { value: new THREE.Matrix4() },
        tShadow2: { value: shadowSlots[0].rts[0].depthTexture }, tPlant2: { value: shadowSlots[0].rts[1].depthTexture },
        tWater: { value: shadowSlots[0].rts[2].depthTexture },
        uShadowMat2: { value: new THREE.Matrix4() }, uShadowMix: { value: 0 },
        uShadowTexel: { value: 1 }, uSun: { value: SUN_DIR },
    };
    const shadowMeshes = new Map(); /* mesh -> [solid, plant] depth materials, null to hide */
    /* The map is drawn SHADOW_SLACK times wider than the view needs and kept
     * until the view leaves it or it gets that much coarser than needed, so
     * turning and panning mostly reuse it instead of drawing the scene twice
     * more every frame. */
    const SHADOW_SLACK = 1.3;
    const shadowBox = { x: 0, y: 0, H: 0 }; /* light-space centre, half size; H 0 = none */
    aimLight();
    const stats = { draws: 0, shadows: 0 };
    /* the eye's shade, eased: two 1x1 targets in turn, each frame's from the last's */
    const EYE_TAU = 0.6; /* s */
    const eyeRTs = [0, 1].map(() => new THREE.WebGLRenderTarget(1, 1, { type: THREE.HalfFloatType, depthBuffer: false }));
    let eyeIdx = 0, eyeLast = 0, eyeShadows = -1;
    app.eyeSettle = 0;
    const eyeAt = new THREE.Vector3(NaN, 0, 0);
    rayU.tEye.value = eyeRTs[0].texture;
    const eyeScene = new THREE.Scene(), eyeCam = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1);
    const eyeU = { tPrev: { value: eyeRTs[0].texture }, uK: { value: 1 } };
    eyeScene.add(new THREE.Mesh(new THREE.PlaneGeometry(2, 2), new THREE.ShaderMaterial({
        uniforms: { tRay: rayU.tRay, uRayMat: rayU.uRayMat, uRayRange: rayU.uRayRange, uEye: { value: camera.position }, ...eyeU },
        vertexShader: "void main() { gl_Position = vec4(position.xy, 0.0, 1.0); }",
        fragmentShader: EYE_GLSL, depthTest: false, depthWrite: false,
    })));
    function renderEyeShade() {
        const now = performance.now();
        /* the first time it starts where it should be */
        const dt = eyeLast ? Math.min(now - eyeLast, 250) / 1000 : Infinity;
        eyeLast = now;
        eyeU.uK.value = 1 - Math.exp(-dt / EYE_TAU);
        eyeU.tPrev.value = eyeRTs[eyeIdx].texture;
        eyeIdx ^= 1;
        renderer.setRenderTarget(eyeRTs[eyeIdx]);
        renderer.render(eyeScene, eyeCam);
        renderer.setRenderTarget(null);
        rayU.tEye.value = eyeRTs[eyeIdx].texture;
        /* the value is on the GPU: after the eye or the sun moves, frames keep
           coming until it has had time to settle */
        if (!eyeAt.equals(camera.position) || app.sunMoving || stats.shadows !== eyeShadows) app.eyeSettle = now + 4000 * EYE_TAU;
        eyeAt.copy(camera.position);
        eyeShadows = stats.shadows;
    }
    function renderShadow() {
        rayU.uRayOn.value = 0;
        if (!shadowOn || !shadowMeshes.size || SUN_DIR.y < 0) return;
        /* bound where the screen corners' rays meet the target's height (or
         * reach 3x the target distance), seen from the sun */
        const d = camera.position.distanceTo(controls.target);
        const lo = new THREE.Vector3(Infinity, Infinity, 0), hi = new THREE.Vector3(-Infinity, -Infinity, 0);
        const p = new THREE.Vector3(), dir = new THREE.Vector3(), uv = new THREE.Vector3();
        for (const [x, y] of [[-1, -1], [1, -1], [-1, 1], [1, 1], [0, 0]]) {
            dir.set(x, y, 0.5).unproject(camera).sub(camera.position).normalize();
            let t = dir.y < -1e-3 ? (controls.target.y - camera.position.y) / dir.y : Infinity;
            t = Math.min(Math.max(t, 0), 3 * d);
            lightUV(p.copy(camera.position).addScaledVector(dir, t), uv);
            lo.min(uv); hi.max(uv);
        }
        const need = THREE.MathUtils.clamp(Math.max(hi.x - lo.x, hi.y - lo.y) * 0.55, 24, 400);
        const y0 = Math.round(controls.target.y);
        const c = lightUV(controls.target, new THREE.Vector3());
        c.x = THREE.MathUtils.clamp((lo.x + hi.x) / 2, c.x - need, c.x + need);
        c.y = THREE.MathUtils.clamp((lo.y + hi.y) / 2, c.y - need, c.y + need);
        const b = shadowBox;
        /* H 0: what is shown changed (tiles came or went), so the shafts' maps
           are stale too; a refit to follow the view leaves those, centred on the eye */
        if (!b.H) for (const sl of rayMaps) sl.deg = NaN;
        if (!(b.H && Math.abs(c.x - b.x) + need <= b.H && Math.abs(c.y - b.y) + need <= b.H &&
              b.H <= need * SHADOW_SLACK * SHADOW_SLACK && Math.abs(y0 - shadowY0) < 16)) {
            if (y0 !== shadowY0) { shadowY0 = y0; lightUV(controls.target, c); }
            const H = need * SHADOW_SLACK, texel = 2 * H / SHADOW_SIZE;
            /* whole texels, so a redrawn map's edges land where the last one's did */
            c.x = Math.round(c.x / texel) * texel;
            c.y = Math.round(c.y / texel) * texel;
            Object.assign(b, { x: c.x, y: c.y, H });
            shadowU.uShadowTexel.value = texel;
            for (const sl of shadowSlots) if (sl) sl.deg = NaN;
        }
        /* the sun's arc at the steps either side while it moves, else where it is */
        const d0 = app.sunMoving ? Math.floor(app.lightDeg / SUN_STEP) * SUN_STEP : app.lightDeg;
        const want = app.sunMoving && app.lightDeg > d0 ? [d0, d0 + SUN_STEP] : [d0];
        const got = want.map(d => shadowSlots.find(sl => sl && sl.deg === d));
        for (let i = 0; i < want.length; i++) {
            if (got[i]) continue;
            let k = shadowSlots.findIndex(sl => sl && !got.includes(sl));
            if (k < 0) { k = shadowSlots.indexOf(null); shadowSlots[k] = { rts: slotRTs(), deg: NaN }; }
            got[i] = shadowSlots[k];
            drawShadowMap(got[i], want[i]);
        }
        shadowU.tShadow.value = got[0].rts[0].depthTexture;
        shadowU.tThin.value = got[0].rts[0].texture;
        shadowU.tPlant.value = got[0].rts[1].depthTexture;
        shadowU.tWater.value = got[0].rts[2].depthTexture;
        shadowU.uShadowMat.value.copy(got[0].mat);
        const g2 = got[1] || got[0];
        shadowU.tShadow2.value = g2.rts[0].depthTexture;
        shadowU.tThin2.value = g2.rts[0].texture;
        shadowU.tPlant2.value = g2.rts[1].depthTexture;
        shadowU.uShadowMat2.value.copy(g2.mat);
        shadowU.uShadowMix.value = got[1] ? (app.lightDeg - d0) / SUN_STEP : 0;
        if (RAYS > 0) { renderRayMaps(d0, want); renderEyeShade(); }
    }
    /* The shafts' own maps (see lightShafts): not the view's, which is fitted
     * to what the view looks at and leaves the air around the eye out of it
     * as the view tilts (lit, by default, so haze came and went), and is laid
     * out on the ground, so that at a low sun air a few blocks up lands far off
     * the map. These look straight down the sun over a square RAY_MAP_H blocks
     * either side of the eye, redrawn once it has moved RAY_MAP_MOVE or the
     * sun a SUN_STEP; two either side while the sun moves, as the view's. */
    const RAY_MAP_SIZE = 1024, RAY_MAP_H = RAY_FAR + 16, RAY_MAP_MOVE = 8;
    const rayCam = new THREE.OrthographicCamera(-RAY_MAP_H, RAY_MAP_H, RAY_MAP_H, -RAY_MAP_H, 0, SHADOW_RANGE);
    const rayMaps = [];
    function rayMapRT() {
        const rt = new THREE.WebGLRenderTarget(RAY_MAP_SIZE, RAY_MAP_SIZE, { format: THREE.RedFormat, generateMipmaps: false });
        rt.depthTexture = new THREE.DepthTexture(RAY_MAP_SIZE, RAY_MAP_SIZE, THREE.UnsignedIntType);
        return rt;
    }
    function drawRayMap(sl, deg) {
        const S = sunAt(deg, new THREE.Vector3());
        /* centred on the eye, snapped so small moves keep the same texels */
        const c = camera.position.clone().divideScalar(RAY_MAP_MOVE).round().multiplyScalar(RAY_MAP_MOVE);
        rayCam.position.copy(c).addScaledVector(S, SHADOW_RANGE / 2);
        rayCam.up.set(0, 1, 0);
        if (Math.abs(S.y) > 0.99) rayCam.up.set(0, 0, 1);
        rayCam.lookAt(c);
        rayCam.updateMatrixWorld();
        sl.mat = new THREE.Matrix4().multiplyMatrices(rayCam.projectionMatrix, rayCam.matrixWorldInverse);
        sl.deg = deg;
        sl.at = c;
        const fog = scene.fog;
        scene.fog = null; sky.visible = false;
        for (const [o] of shadowMeshes) { o.userData.mat = o.material; o.userData.vis = o.visible; }
        for (const [o] of shadowMeshes) if (o.userData.face && o.visible)
            o.material.name === "OPAQUE" ? app.faceCull(o, null, [S.x, S.y, S.z]) : o.geometry.setDrawRange(o.userData.face.q0 * 6, o.userData.quads * 6);
        for (const [o, dm] of shadowMeshes) { o.visible = o.userData.vis && !!dm[0]; if (dm[0]) o.material = dm[0]; }
        renderer.setRenderTarget(sl.rt);
        renderer.render(scene, rayCam);
        renderer.setRenderTarget(null);
        for (const [o] of shadowMeshes) { o.material = o.userData.mat; o.visible = o.userData.vis; }
        scene.fog = fog; sky.visible = true;
    }
    /* tests: the eye's shade as the shafts read it */
    function eyeShade() {
        const b = new Uint16Array(4);
        renderer.readRenderTargetPixels(eyeRTs[eyeIdx], 0, 0, 1, 1, b);
        return THREE.DataUtils.fromHalfFloat(b[0]);
    }
    function renderRayMaps(d0, want) {
        const near = sl => sl.at && Math.max(Math.abs(sl.at.x - camera.position.x), Math.abs(sl.at.y - camera.position.y),
                                             Math.abs(sl.at.z - camera.position.z)) <= RAY_MAP_MOVE;
        const got = want.map(d => rayMaps.find(sl => sl.deg === d && near(sl)));
        for (let i = 0; i < want.length; i++) {
            if (got[i]) continue;
            let sl = rayMaps.find(m => !got.includes(m));
            if (!sl && rayMaps.length < 3) { sl = { rt: rayMapRT(), deg: NaN }; rayMaps.push(sl); }
            if (!sl) sl = rayMaps.find(m => !got.includes(m));
            got[i] = sl;
            drawRayMap(sl, want[i]);
            stats.rayMaps = (stats.rayMaps || 0) + 1;
        }
        rayU.tRay.value = got[0].rt.depthTexture;
        rayU.uRayMat.value.copy(got[0].mat);
        const g2 = got[1] || got[0];
        rayU.tRay2.value = g2.rt.depthTexture;
        rayU.uRayMat2.value.copy(g2.mat);
        rayU.uRayMix.value = got[1] ? (app.lightDeg - d0) / SUN_STEP : 0;
        rayU.uRayRange.value = SHADOW_RANGE;
        rayU.uRayOn.value = 1;
    }
    /* The map pair in slot sl for the sun at deg, over shadowBox:
     * u = x - (y - y0) kx, v = z - (y - y0) kz around its centre, depth from
     * SHADOW_RANGE / 2 before the target along the ray. */
    function drawShadowMap(sl, deg) {
        stats.shadows++;
        const S = sunAt(deg, new THREE.Vector3()), k = shearOf(S, new THREE.Vector2());
        const b = shadowBox, H = b.H, R = SHADOW_RANGE;
        const dz = controls.target.dot(S) + R / 2;
        const M = new THREE.Matrix4().set(
            1 / H, -k.x / H, 0, (k.x * shadowY0 - b.x) / H,
            0, -k.y / H, 1 / H, (k.y * shadowY0 - b.y) / H,
            -2 * S.x / R, -2 * S.y / R, -2 * S.z / R, 2 * dz / R - 1,
            0, 0, 0, 1);
        /* the same handedness as a camera looking down the ray, or the faces
           towards the sun would be culled instead of those away from it */
        if (M.determinant() > 0) for (const i of [1, 5, 9, 13]) M.elements[i] = -M.elements[i];
        lightCam.projectionMatrix.copy(M);
        lightCam.projectionMatrixInverse.copy(M).invert();
        sl.mat = M;
        sl.deg = deg;
        const fog = scene.fog;
        scene.fog = null; sky.visible = false;
        for (const [o] of shadowMeshes) { o.userData.mat = o.material; o.userData.vis = o.visible; }
        /* sunward faces only, of the one-sided solid map; cut-outs cast from both sides */
        for (const [o] of shadowMeshes) if (o.userData.face && o.visible)
            o.material.name === "OPAQUE" ? app.faceCull(o, null, [SUN_DIR.x, SUN_DIR.y, SUN_DIR.z]) : o.geometry.setDrawRange(o.userData.face.q0 * 6, o.userData.quads * 6);
        for (const [i, rt] of sl.rts.entries()) {
            for (const [o, dm] of shadowMeshes) { o.visible = o.userData.vis && !!dm[i]; if (dm[i]) o.material = dm[i]; }
            renderer.setRenderTarget(rt);
            renderer.render(scene, lightCam);
        }
        renderer.setRenderTarget(null);
        for (const [o] of shadowMeshes) { o.material = o.userData.mat; o.visible = o.userData.vis; }
        scene.fog = fog; sky.visible = true;
    }

    Object.assign(app, { NEAR0, NEAR1, PENUMBRA, PLANT_BIAS, PLANT_CAST, SHADOW_RANGE, SHADOW_SIZE, THIN_NEAR0, THIN_NEAR1,
                       aimLight, eyeShade, renderShadow, shadowBox, shadowMeshes, shadowOn, shadowU, stats });
}
