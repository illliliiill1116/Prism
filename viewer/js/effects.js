/*
 * Post-processing, with FX on: the scene drawn in two parts, light shafts,
 * depth of field, grading, bloom and tilt-shift.
 *
 * Effects (the FX button, F key, ?fx=0/1; remembered per browser): colour
 * grading with a vignette (water excluded; ?sat= scales its saturation),
 * bloom (?bloom= its strength), and tilt-shift blur away from the
 * screen's middle band, strongest at middle zoom looking down. The grade
 * also clears NaN pixels, which bloom would smear into black squares.
 */
import * as THREE from "three";
import { EffectComposer } from "three/addons/postprocessing/EffectComposer.js";
import { UnrealBloomPass } from "three/addons/postprocessing/UnrealBloomPass.js";
import { ShaderPass } from "three/addons/postprocessing/ShaderPass.js";
import { OutputPass } from "three/addons/postprocessing/OutputPass.js";
import { Pass, FullScreenQuad } from "three/addons/postprocessing/Pass.js";

export function setupEffects(app) {
    const { FOV_STD, RAYS, RAY_FAR, RAY_GLSL, camera, controls, q, rayU, renderer, scene, waterU } = app;
    /* ---- effects ---- */

    const FULLSCREEN_VS = `varying vec2 vUv;
void main() { vUv = uv; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }`;

    /* a large photo being drawn, tile by tile (see shoot) */
    app.shot = null;
    /* Where the drawn frame lies in the picture, in its uv: (0, 0, 1, 1) but
     * for a tile of a large photo (see shoot) */
    const shotFrame = { value: new THREE.Vector4(0, 0, 1, 1) };
    /* One direction of a separable blur whose radius grows with the distance
     * from a horizontal band in the middle of the screen: a miniature look. */
    const TiltShader = {
        uniforms: { tDiffuse: { value: null }, uStep: { value: new THREE.Vector2() },
                    uFocus: { value: 0.5 }, uBand: { value: 0.12 }, uMax: { value: 6.0 },
                    uFrame: shotFrame, uTaps: { value: 7 } },
        vertexShader: FULLSCREEN_VS,
        fragmentShader: `uniform sampler2D tDiffuse;
uniform vec2 uStep;
uniform float uFocus, uBand, uMax, uTaps;
uniform vec4 uFrame;
varying vec2 vUv;
void main() {
	/* radius in CSS pixels; taps at most one pixel apart, so no dot grid */
	float r = uMax * smoothstep(uBand, uBand + 0.35, abs(uFrame.y + vUv.y * uFrame.w - uFocus));
	if (r < 0.25) { gl_FragColor = texture2D(tDiffuse, vUv); return; }
	vec4 c = vec4(0.0);
	float wsum = 0.0;
	for (int i = 0; i <= 56; i++) {
		if (float(i) > 2.0 * uTaps) break;
		float x = (float(i) - uTaps) / uTaps, w = exp(-3.0 * x * x);
		c += texture2D(tDiffuse, vUv + uStep * (x * r)) * w;
		wsum += w;
	}
	gl_FragColor = c / wsum;
}`,
    };

    /* Right after the scene, before bloom: clears NaN/Inf (bloom would smear a
     * NaN pixel into a black square), then grades in display space. Vivid
     * colours (roofs, flowers) are eased back while muted ones (grass, stone)
     * get a touch more, then a soft S-curve, split toning and a vignette.
     * Water is left as it is: its material writes alpha 0 into the target.
     * First the camera's exposure and white balance, a gain on linear light. */
    const GradeShader = {
        uniforms: { tDiffuse: { value: null }, uAspect: { value: 1 }, uSat: { value: 1 }, uGain: { value: new THREE.Vector3(1, 1, 1) },
                    uFrame: shotFrame },
        vertexShader: FULLSCREEN_VS,
        fragmentShader: `uniform sampler2D tDiffuse;
uniform float uAspect, uSat;
uniform vec3 uGain;
uniform vec4 uFrame;
varying vec2 vUv;
void main() {
	vec4 t = texture2D(tDiffuse, vUv);
	if (any(isnan(t))) { gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
	vec3 lin = clamp(t.rgb * uGain, 0.0, 64.0);
	vec3 c0 = pow(lin, vec3(1.0 / 2.2));
	vec3 c = min(c0, 1.0), over = c0 - c;
	float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
	float chroma = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
	float sat = mix(1.06, 0.80, smoothstep(0.18, 0.55, chroma)) * uSat;
	c = clamp(l + (c - l) * sat, 0.0, 1.0);
	c = mix(c, c * c * (3.0 - 2.0 * c), 0.15);
	c += mix(vec3(-0.006, 0.0, 0.015), vec3(0.015, 0.006, -0.01), l);
	c = mix(max(c, 0.0) + over, c0, 1.0 - clamp(t.a, 0.0, 1.0));
	vec2 d = (uFrame.xy + vUv * uFrame.zw - 0.5) * vec2(uAspect, 1.0);
	c *= mix(1.0, 0.72, smoothstep(0.35, 1.0, length(d)));
	gl_FragColor = vec4(pow(c, vec3(2.2)), 1.0);
}`,
    };

    /* the scene's depth too, for ShaftPass */
    const composerRT = new THREE.WebGLRenderTarget(1, 1, { type: THREE.HalfFloatType, samples: 4 });
    composerRT.depthTexture = new THREE.DepthTexture(1, 1, THREE.UnsignedIntType);
    const composer = new EffectComposer(renderer, composerRT);
    /* The scene in two parts (see WATER_ABS): layer 0, then layer 1 (water,
     * glass) over it without clearing. Each render resolves the multisampled
     * target into its texture and depth texture, which the second part reads
     * while it draws into the multisampled buffers - no feedback loop. */
    const LAYER_OVER = 1;
    const dofDepth = { value: null };
    /* the first part of a screen-sized frame, kept for a photo's tiles: its
     * colour, and its depth as view-space z (see WATER_SCENE_GLSL) */
    const shotGrab = {
        want: false,
        col: new THREE.WebGLRenderTarget(1, 1, { type: THREE.HalfFloatType, depthBuffer: false }),
        z: new THREE.WebGLRenderTarget(1, 1, { type: THREE.FloatType, depthBuffer: false }),
        quad: null, copy: null, toZ: null,
        take(renderer, from) {
            if (!this.quad) {
                this.quad = new FullScreenQuad(null);
                this.copy = new THREE.ShaderMaterial({ uniforms: { t: { value: null } }, vertexShader: FULLSCREEN_VS,
                    fragmentShader: "uniform sampler2D t; varying vec2 vUv; void main() { gl_FragColor = texture2D(t, vUv); }" });
                this.toZ = new THREE.ShaderMaterial({ uniforms: { t: { value: null }, uProj: waterU.uProj }, vertexShader: FULLSCREEN_VS,
                    fragmentShader: `uniform sampler2D t; uniform mat4 uProj; varying vec2 vUv;
void main() { float d = texture2D(t, vUv).r; gl_FragColor = vec4(uProj[3][2] / (1.0 - 2.0 * d - uProj[2][2]), 0.0, 0.0, 1.0); }` });
            }
            const w = from.width, h = from.height;
            this.col.setSize(w, h); this.z.setSize(w, h);
            this.copy.uniforms.t.value = from.texture;
            this.quad.material = this.copy; renderer.setRenderTarget(this.col); this.quad.render(renderer);
            this.toZ.uniforms.t.value = from.depthTexture;
            this.quad.material = this.toZ; renderer.setRenderTarget(this.z); this.quad.render(renderer);
            waterU.tFull.value = this.col.texture;
            waterU.tFullZ.value = this.z.texture;
            waterU.uProjFull.value.copy(camera.projectionMatrix);
        },
        free() { this.col.setSize(1, 1); this.z.setSize(1, 1); },
    };
    class SplitRenderPass extends Pass {
        constructor() { super(); this.needsSwap = false; }
        render(renderer, writeBuffer, readBuffer) {
            renderer.setRenderTarget(readBuffer);
            camera.layers.set(0);
            renderer.render(scene, camera);
            if (shotGrab.want) { shotGrab.take(renderer, readBuffer); renderer.setRenderTarget(readBuffer); }
            waterU.tScene.value = readBuffer.texture;
            waterU.tSceneDepth.value = dofDepth.value = readBuffer.depthTexture;
            waterU.uSceneOn.value = 1;
            const ac = renderer.autoClear;
            renderer.autoClear = false;
            camera.layers.set(LAYER_OVER);
            renderer.render(scene, camera);
            renderer.autoClear = ac;
            camera.layers.enableAll();
            waterU.uSceneOn.value = 0;
            if (dofPass.enabled) dofPass.radii(renderer);
        }
    }
    composer.addPass(new SplitRenderPass());

    /* Light shafts as a pass (with FX; without, the materials' haze adds them,
     * grainier): marched at half resolution from the scene's depth, blurred
     * along x then y with taps weighted by how near their depth is (so the
     * blur stays off silhouettes), and added back at full resolution, each
     * pixel taking the half-resolution texels around it by the same depth
     * test. A quarter of the marches, and the per-pixel jitter's grain is
     * gone into the blur. */
    const SHAFT_TAPS_MAX = 24; /* a side, for the widest blur; taps then 1-2 texels apart */
    class ShaftPass extends Pass {
        constructor() {
            super();
            const rt = () => new THREE.WebGLRenderTarget(1, 1, { type: THREE.HalfFloatType, depthBuffer: false });
            this.a = rt(); this.b = rt();
            const depthTo = `
uniform sampler2D tDepth;
uniform mat4 uInvVP;
uniform vec3 uCam;
/* the world point at uv, and whether it is sky (nothing drawn) */
vec3 worldAt(vec2 uv, out bool sky) {
	float d = texture2D(tDepth, uv).r;
	sky = d >= 0.999999;
	vec4 w = uInvVP * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
	return w.xyz / w.w;
}`;
            this.march = new THREE.ShaderMaterial({
                uniforms: { ...rayU, tDepth: { value: null }, uInvVP: { value: new THREE.Matrix4() }, uCam: { value: new THREE.Vector3() } },
                vertexShader: FULLSCREEN_VS,
                fragmentShader: `varying vec2 vUv;
${RAY_GLSL}
${depthTo}
void main() {
	bool sky;
	vec3 p = worldAt(vUv, sky), dir = normalize(p - uCam);
	if (sky) p = uCam + dir * ${RAY_FAR}.0;
	gl_FragColor = vec4(lightShafts(uCam, p), length(p - uCam));
}`,
            });
            this.blur = new THREE.ShaderMaterial({
                uniforms: { tShaft: { value: null }, uStep: { value: new THREE.Vector2() }, uSpread: { value: 1 }, uTaps: { value: 4 } },
                vertexShader: FULLSCREEN_VS,
                fragmentShader: `varying vec2 vUv;
uniform sampler2D tShaft;
uniform vec2 uStep;
uniform float uSpread, uTaps;
void main() {
	vec4 c = texture2D(tShaft, vUv);
	vec3 sum = c.rgb;
	float ws = 1.0, tol = 0.5 + 0.05 * c.a;
	for (int i = 1; i <= ${SHAFT_TAPS_MAX}; i++) {
		if (float(i) > uTaps) break;
		/* texels out; the Gaussian is 2 texels wide times uSpread */
		float x = float(i) * 4.0 * uSpread / uTaps, g = exp(-x * x / (8.0 * uSpread * uSpread));
		for (int s = -1; s <= 1; s += 2) {
			vec4 t = texture2D(tShaft, vUv + uStep * x * float(s));
			float w = g * exp(-abs(t.a - c.a) / tol);
			sum += t.rgb * w; ws += w;
		}
	}
	gl_FragColor = vec4(sum / ws, c.a);
}`,
            });
            this.add = new THREE.ShaderMaterial({
                uniforms: { tDiffuse: { value: null }, tShaft: { value: null }, tDepth: { value: null }, uInvVP: { value: new THREE.Matrix4() },
                            uCam: { value: new THREE.Vector3() }, uHalf: { value: new THREE.Vector2() } },
                vertexShader: FULLSCREEN_VS,
                fragmentShader: `varying vec2 vUv;
uniform sampler2D tDiffuse, tShaft;
uniform vec2 uHalf;
${depthTo}
void main() {
	vec4 c = texture2D(tDiffuse, vUv);
	bool sky;
	vec3 p = worldAt(vUv, sky);
	float d = sky ? ${RAY_FAR}.0 : length(p - uCam), tol = 0.5 + 0.05 * d;
	/* the four half-resolution texels around, bilinear by position and kept by depth */
	vec2 t = vUv * uHalf - 0.5, f = fract(t), b = (floor(t) + 0.5) / uHalf;
	vec3 sum = vec3(0.0);
	float ws = 0.0;
	for (int j = 0; j < 2; j++)
		for (int i = 0; i < 2; i++) {
			vec4 s = texture2D(tShaft, b + vec2(i, j) / uHalf);
			float w = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y) * exp(-abs(s.a - d) / tol) + 1e-4;
			sum += s.rgb * w; ws += w;
		}
	gl_FragColor = vec4(c.rgb + sum / ws, c.a);
}`,
            });
            this.quad = new FullScreenQuad(null);
        }
        /* half the frame's resolution; for a photo's tile 1/2k, so the march's
           grain and the ray map's steps are blurred over the same share of the
           picture as on screen (at 1/2 they came out as fine streaks) */
        setSize(w, h) { this.w = w; this.h = h; this.fit(); }
        fit() {
            const div = 2 * (app.shot ? app.shot.k : 1);
            const hw = Math.max(1, Math.ceil(this.w / div)), hh = Math.max(1, Math.ceil(this.h / div));
            if (this.a.width === hw && this.a.height === hh) return;
            this.a.setSize(hw, hh); this.b.setSize(hw, hh);
            this.add.uniforms.uHalf.value.set(hw, hh);
        }
        render(renderer, writeBuffer, readBuffer) {
            this.fit();
            const inv = new THREE.Matrix4().multiplyMatrices(camera.matrixWorld, camera.projectionMatrixInverse);
            for (const m of [this.march, this.add]) {
                m.uniforms.tDepth.value = readBuffer.depthTexture;
                m.uniforms.uInvVP.value.copy(inv);
                m.uniforms.uCam.value.copy(camera.position);
            }
            const go = (mat, target) => { this.quad.material = mat; renderer.setRenderTarget(target); this.quad.render(renderer); };
            go(this.march, this.a);
            /* the blur by angle, not pixels: as wide in the view at any focal
               length (through a long lens 8 pixels are a narrow angle, and the
               shadows of trunks and posts in the mist stood out as even bands) */
            const spread = Math.max(1, Math.tan(THREE.MathUtils.degToRad(FOV_STD) / 2) / Math.tan(THREE.MathUtils.degToRad(camera.fov) / 2));
            this.blur.uniforms.uSpread.value = spread;
            this.blur.uniforms.uTaps.value = Math.min(SHAFT_TAPS_MAX, Math.ceil(4 * spread));
            this.blur.uniforms.tShaft.value = this.a.texture;
            this.blur.uniforms.uStep.value.set(1 / this.a.width, 0);
            go(this.blur, this.b);
            this.blur.uniforms.tShaft.value = this.b.texture;
            this.blur.uniforms.uStep.value.set(0, 1 / this.a.height);
            go(this.blur, this.a);
            this.add.uniforms.tDiffuse.value = readBuffer.texture;
            this.add.uniforms.tShaft.value = this.a.texture;
            go(this.add, this.renderToScreen ? null : writeBuffer);
        }
    }
    const shaftPass = new ShaftPass();
    shaftPass.enabled = RAYS > 0;
    composer.addPass(shaftPass);
    /* Depth of field of a thin lens (the camera panel's aperture), on linear
     * light before the grade so highlights spread into bright discs. A first
     * pass turns the scene's depth into each pixel's blur radius - from the
     * depth under the focus point, read in the shader, so focusing costs no
     * read-back - into a target of its own, right after the scene is drawn
     * (the composer's two targets share one depth texture, which the next
     * pass to draw clears). Two separable gathers follow;
     * a tap counts if its own radius reaches the pixel, and one behind the
     * pixel only as far as the pixel's own radius, so a sharp subject keeps
     * its edge against a blurred background while a blurred foreground still
     * spreads over it. Radii in CSS pixels, at most DOF_MAX. */
    const DOF_MAX = 24;
    class DofPass extends Pass {
        constructor() {
            super();
            const rt = () => new THREE.WebGLRenderTarget(1, 1, { type: THREE.HalfFloatType, depthBuffer: false });
            this.z = rt(); this.a = rt();
            this.zMat = new THREE.ShaderMaterial({
                uniforms: { tDepth: dofDepth, uFocusUv: { value: new THREE.Vector2(0.5, 0.5) }, uFocusZ: { value: 0 },
                            uNear: { value: 1 }, uFar: { value: 1 }, uK: { value: 0 }, uMaxR: { value: DOF_MAX } },
                vertexShader: FULLSCREEN_VS,
                fragmentShader: `uniform sampler2D tDepth;
uniform vec2 uFocusUv;
uniform float uFocusZ, uNear, uFar, uK, uMaxR;
varying vec2 vUv;
float lin(float d) { return uNear * uFar / (uFar - d * (uFar - uNear)); }
void main() {
	/* a photo's tile may not hold the focus point: then its depth is given */
	float zf = uFocusZ > 0.0 ? uFocusZ : lin(texture2D(tDepth, uFocusUv).r), z = lin(texture2D(tDepth, vUv).r);
	gl_FragColor = vec4(min(uK * abs(1.0 / zf - 1.0 / z), uMaxR), z, 0.0, 1.0);
}`,
            });
            this.blur = new THREE.ShaderMaterial({
                uniforms: { tDiffuse: { value: null }, tCoc: { value: this.z.texture }, uStep: { value: new THREE.Vector2() },
                            uMaxR: { value: DOF_MAX }, uTaps: { value: 10 } },
                vertexShader: FULLSCREEN_VS,
                fragmentShader: `uniform sampler2D tDiffuse, tCoc;
uniform vec2 uStep;
uniform float uMaxR, uTaps;
varying vec2 vUv;
void main() {
	vec2 c = texture2D(tCoc, vUv).rg;
	/* the reach: this pixel's radius or a blurred neighbour's spreading over it */
	float R = c.r;
	for (int k = 1; k <= 4; k++) {
		float o = float(k) * uMaxR * 0.25;
		R = max(R, max(texture2D(tCoc, vUv + uStep * o).r, texture2D(tCoc, vUv - uStep * o).r));
	}
	if (R < 0.5) { gl_FragColor = texture2D(tDiffuse, vUv); return; }
	vec4 sum = vec4(0.0);
	float ws = 0.0;
	for (int i = 0; i <= 80; i++) {
		if (float(i) > 2.0 * uTaps) break;
		float x = (float(i) - uTaps) / uTaps * R;
		vec2 uv = vUv + uStep * x;
		vec2 t = texture2D(tCoc, uv).rg;
		float r = t.g > c.g ? min(t.r, c.r) : t.r;
		float w = clamp(r - abs(x) + 1.0, 0.0, 1.0) / max(r, 1.0);
		sum += texture2D(tDiffuse, uv) * w;
		ws += w;
	}
	gl_FragColor = sum / ws;
}`,
            });
            this.quad = new FullScreenQuad(null);
        }
        setSize(w, h) { this.z.setSize(w, h); this.a.setSize(w, h); }
        radii(renderer) {
            const u = this.zMat.uniforms;
            u.uNear.value = camera.near; u.uFar.value = camera.far;
            this.quad.material = this.zMat;
            renderer.setRenderTarget(this.z);
            this.quad.render(renderer);
        }
        render(renderer, writeBuffer, readBuffer) {
            const go = (mat, target) => { this.quad.material = mat; renderer.setRenderTarget(target); this.quad.render(renderer); };
            this.blur.uniforms.tDiffuse.value = readBuffer.texture;
            this.blur.uniforms.uStep.value.set(1 / app.width, 0);
            go(this.blur, this.a);
            this.blur.uniforms.tDiffuse.value = this.a.texture;
            this.blur.uniforms.uStep.value.set(0, 1 / app.height);
            go(this.blur, this.renderToScreen ? null : writeBuffer);
        }
    }
    const dofPass = new DofPass();
    dofPass.enabled = false;
    composer.addPass(dofPass);
    const grade = new ShaderPass(GradeShader);
    grade.uniforms.uSat.value = q.has("sat") ? Number(q.get("sat")) : 0.8;
    composer.addPass(grade);
    const bloom = new UnrealBloomPass(new THREE.Vector2(app.width, app.height), 0.22, 0.5, 0.85);
    if (q.has("bloom")) bloom.strength = Number(q.get("bloom"));
    composer.addPass(bloom);
    /* In a photo's tiles the bloom is the whole view's, kept from a screen-sized
     * frame (shotBloom) and added over the tile's part of it: bloom's glow is
     * wide, and from a tile alone each tile glowed by what it held of the
     * bright sky, in steps along the tile edges. */
    const shotBloom = {
        rt: new THREE.WebGLRenderTarget(1, 1, { type: THREE.HalfFloatType, depthBuffer: false }),
        keep(renderer) {
            const src = bloom.renderTargetsHorizontal[0];
            this.rt.setSize(src.width, src.height);
            bloomView.mat.uniforms.t.value = src.texture;
            bloomView.mat.blending = THREE.NoBlending;
            bloomView.quad.material = bloomView.mat;
            renderer.setRenderTarget(this.rt);
            bloomView.quad.render(renderer);
            bloomView.mat.blending = THREE.AdditiveBlending;
            bloomView.mat.uniforms.t.value = this.rt.texture;
        },
        free() { this.rt.setSize(1, 1); },
    };
    class BloomViewPass extends Pass {
        constructor() {
            super();
            this.needsSwap = false;
            this.mat = new THREE.ShaderMaterial({
                uniforms: { t: { value: null }, uFrame: shotFrame },
                vertexShader: FULLSCREEN_VS,
                fragmentShader: `uniform sampler2D t;
uniform vec4 uFrame;
varying vec2 vUv;
void main() { gl_FragColor = texture2D(t, uFrame.xy + vUv * uFrame.zw); }`,
                blending: THREE.AdditiveBlending, depthTest: false, depthWrite: false, transparent: true,
            });
            this.quad = new FullScreenQuad(this.mat);
        }
        render(renderer, writeBuffer, readBuffer) {
            const ac = renderer.autoClear;
            renderer.autoClear = false; /* over the frame */
            renderer.setRenderTarget(this.renderToScreen ? null : readBuffer);
            this.quad.render(renderer);
            renderer.autoClear = ac;
        }
    }
    const bloomView = new BloomViewPass();
    bloomView.enabled = false;
    composer.addPass(bloomView);
    const tiltH = new ShaderPass(TiltShader), tiltV = new ShaderPass(TiltShader);
    composer.addPass(tiltH);
    composer.addPass(tiltV);
    /* ShaderPass clones its uniforms: share the photo's frame again, or each
       tile drew its own vignette (dark bands at every tile edge) */
    for (const p of [grade, tiltH, tiltV]) p.uniforms.uFrame = shotFrame;
    composer.addPass(new OutputPass());
    function sizeEffects() {
        const pr = renderer.getPixelRatio();
        composer.setPixelRatio(pr);
        composer.setSize(app.width, app.height);
        tiltH.uniforms.uStep.value.set(1 / app.width, 0);
        tiltV.uniforms.uStep.value.set(0, 1 / app.height);
        grade.uniforms.uAspect.value = app.width / app.height;
    }
    sizeEffects();

    app.fx = !q.has("fx") || q.get("fx") !== "0";
    /* The miniature blur only reads right from a middle distance looking down:
     * up close it smears what is in view, far out the map is already small,
     * and near the horizon it blurs the sky. Full radius from 120 to 450
     * blocks out, fading to none at 40 and 1000, and with a shallow pitch. */
    const TILT_MAX = 6.0;
    function fitTilt() {
        const d = app.lookDist();
        const pitch = Math.asin(Math.min(1, (camera.position.y - controls.target.y) / camera.position.distanceTo(controls.target)));
        const s = THREE.MathUtils.smoothstep(d, 40, 120) * (1 - THREE.MathUtils.smoothstep(d, 450, 1000))
                * THREE.MathUtils.smoothstep(pitch, 0.15, 0.45);
        const r = TILT_MAX * s;
        tiltH.uniforms.uMax.value = tiltV.uniforms.uMax.value = r;
        tiltH.enabled = tiltV.enabled = r >= 0.25 && !dofPass.enabled;
    }
    function setFx(on) {
        app.fx = on;
        app.applyCam(); /* its note */
        app.draw();
    }
    app.on(app.keys, "keydown", e => { if (e.code === "KeyF" && !e.ctrlKey && !e.metaKey && !e.altKey) setFx(!app.fx); });

    Object.assign(app, { DOF_MAX, LAYER_OVER, bloom, bloomView, composer, dofPass, fitTilt, grade, setFx, shaftPass, shotBloom,
                       shotFrame, shotGrab, sizeEffects, tiltH, tiltV });
}
