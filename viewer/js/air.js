/*
 * Haze and light shafts.
 *
 * Light shafts (?rays=, 0 = none): the haze near the eye lit where the
 * sun's shadow map reaches it, so beams come through gaps in trees.
 */
import * as THREE from "three";

export function setupAir(app) {
    const { SUN_DIR, camera, controls, q, scene } = app;
    /* Light shafts: the haze lit by the sun where the shadow map says the sun
     * reaches it, and dark in the shadows of trees and buildings. Along each
     * view ray, RAY_STEPS steps (jittered per pixel, closer together near the
     * eye, where a trunk's or a leaf gap's shaft is wide enough to resolve) out
     * to RAY_FAR blocks:
     * each lit step adds the light the air there scatters towards the eye,
     * forward mostly (Henyey-Greenstein), so shafts show looking towards the
     * sun and fade looking away. The air is thicker in the ground mist (see
     * MIST_Y). Out of the map's reach, or with no map, the air counts as lit.
     * Only with the eye in shade (eyeShade): shafts are lit air against dark
     * air, seen from under trees or in a street's shadow; from out in the open
     * sun all that air is lit, which is just the haze the fog already draws -
     * counted again it laid a veil over the whole view from afar that went
     * away as the eye came down into the shade.
     * In the haze of every map pixel (fog_fragment) and in the sky behind them.
     * ?rays= its strength, 0 = none. */
    /* MIST_Y: the ground mist's level, the map's water surface (map.json's
       "sea", see map.js: uMistY); 62, the top water block of a world at the
       game's sea level, until a map says */
    const MIST_Y = 62, MIST_H = 14, MIST = q.has("mist") ? Number(q.get("mist")) : 1; /* see the haze */
    const RAYS = q.has("rays") ? Number(q.get("rays")) : 1;
    const RAY_STEPS = 16, RAY_FAR = 64, RAY_DENS = 0.00072;
    const rayU = {
        tRay: { value: null }, uRayMat: { value: new THREE.Matrix4() }, uRayOn: { value: 0 }, uRayRange: { value: 1 }, tEye: { value: null },
        tRay2: { value: null }, uRayMat2: { value: new THREE.Matrix4() }, uRayMix: { value: 0 },
        uRayHere: { value: 1 }, /* 1: in the materials' haze (FX off); 0: ShaftPass does it */
        uRayCol: { value: new THREE.Color(0, 0, 0) }, uRaySun: { value: SUN_DIR },
        uMistY: { value: MIST_Y },
    };
    /* 0..1, how shaded the eye is (see eyeShade): worked out once a frame into a
     * 1x1 target (renderEyeShade), the same for every pixel */
    const EYE_GLSL = `uniform sampler2D tRay, tPrev;
uniform mat4 uRayMat;
uniform float uRayRange, uK;
uniform vec3 uEye;
/* 0..1, how shaded the eye is: the map over a 3x3x3 grid 3 blocks apart
   around it, each point fading over a block of depth, so a step across a
   leaf's edge moves it a little, not all the way; the same for every pixel */
float eyeShade() {
	float lit = 0.0;
	for (int i = 0; i < 27; i++) {
		vec3 o = 3.0 * vec3(float(i % 3) - 1.0, float((i / 3) % 3) - 1.0, float(i / 9) - 1.0);
		vec3 sp = (uRayMat * vec4(uEye + o, 1.0)).xyz * 0.5 + 0.5;
		if (any(lessThan(sp.xy, vec2(0.0))) || any(greaterThan(sp.xy, vec2(1.0)))) { lit += 1.0; continue; }
		lit += 1.0 - smoothstep(0.3, 1.3, (sp.z - texture2D(tRay, sp.xy).r) * uRayRange);
	}
	return smoothstep(0.1, 0.75, 1.0 - lit / 27.0);
}
/* and eased towards over EYE_TAU, as eyes adjust: the shafts and their
   haze fade in and out instead of flickering as the eye moves along the
   edge of a shade */
void main() { gl_FragColor = vec4(mix(texture2D(tPrev, vec2(0.5)).r, eyeShade(), uK), 0.0, 0.0, 1.0); }`;
    const RAY_GLSL = `uniform sampler2D tRay, tRay2, tEye;
uniform mat4 uRayMat, uRayMat2;
uniform float uRayOn, uRayRange, uRayMix, uRayHere, uMistY;
/* 1 where the sun reaches p, by one map of the pair */
float rayVis(sampler2D map, mat4 m, vec3 p) {
	vec3 sp = (m * vec4(p, 1.0)).xyz * 0.5 + 0.5;
	if (any(lessThan(sp.xy, vec2(0.0))) || any(greaterThan(sp.xy, vec2(1.0)))) return 1.0;
	return step((sp.z - texture2D(map, sp.xy).r) * uRayRange, 0.6);
}
uniform vec3 uRayCol, uRaySun;
/* the mean over this pixel's 2x2 block, from the screen derivatives: the
   four pixels' steps interleave (see lightShafts), so together they march
   at 4x the steps - far less grain, for no more map reads */
vec3 quadMean(vec3 v) {
	vec2 par = mod(floor(gl_FragCoord.xy), 2.0) * -2.0 + 1.0; /* towards the other pixel: +1 even, -1 odd */
	v += 0.5 * dFdx(v) * par.x;
	return v + 0.5 * dFdy(v) * par.y;
}
vec3 lightShafts(vec3 p0, vec3 p1) {
	vec3 rd = p1 - p0;
	float len = length(rd);
	if (len < 1e-3 || dot(uRayCol, vec3(1.0)) <= 0.0) return vec3(0.0);
	vec3 dir = rd / len;
	len = min(len, ${RAY_FAR}.0);
	float mu = dot(dir, uRaySun), g = 0.6;
	float hg = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * mu, 1.5);
	float phase = min(0.25 + 0.75 * hg, 3.0);
	/* looking away from the light the lit air is too faint for its shadows
	   to show: there it is all counted as lit, without the map */
	float useMap = uRayOn * smoothstep(0.45, 0.7, phase);
	if (uRayOn < 0.5) return vec3(0.0);
	float shade = texture2D(tEye, vec2(0.5)).r;
	if (shade <= 0.0) return vec3(0.0);
	/* the four pixels of a 2x2 block start a quarter step apart, so with
	   quadMean the block takes 4x the steps evenly spaced; the block's own
	   offset is interleaved gradient noise over blocks */
	vec2 q = floor(gl_FragCoord.xy * 0.5), par = mod(floor(gl_FragCoord.xy), 2.0);
	float j = fract(52.9829189 * fract(dot(q, vec2(0.06711056, 0.00583715))) + (par.x + 2.0 * par.y) * 0.25);
	float sum = 0.0;
	for (int i = 0; i < ${RAY_STEPS}; i++) {
		/* t = len x^2: the step's length is its share of the ray */
		float x = (float(i) + j) / ${RAY_STEPS}.0, ds = len * 2.0 * x / ${RAY_STEPS}.0;
		vec3 p = p0 + dir * (len * x * x);
		float vis = 1.0;
		if (useMap > 0.0) {
			/* while the sun moves, the maps a SUN_STEP either side, blended */
			float v = rayVis(tRay, uRayMat, p);
			if (uRayMix > 0.0) v = mix(v, rayVis(tRay2, uRayMat2, p), uRayMix);
			vis = mix(1.0, v, useMap);
		}
		float mist = min(exp(-clamp((p.y - uMistY) / ${MIST_H.toFixed(1)}, -3.0, 40.0)), 8.0);
		sum += vis * (1.0 + ${(5 * MIST).toFixed(2)} * mist) * ds;
	}
	return uRayCol * phase * sum * shade * ${(RAY_DENS * RAYS).toFixed(6)};
}`;
    /* Haze: distance fog (past what the camera is looking at, however far out
     * it is) plus ground mist - air denser below MIST_Y + a few MIST_H,
     * integrated along each view ray, so valleys and water fill with it while
     * the map seen from above stays clear (?mist=, 0 = none). Hazed colour
     * also loses saturation before it turns to the horizon colour. */
    scene.fog = new THREE.Fog(0xffffff, 300, 1000);
    /* three's fog chunks, replaced in each material that takes the light
       (patchFog): set in THREE.ShaderChunk they would reach every three.js
       material on the page, another viewer's too */
    const FOG = {};
    FOG.fog_pars_vertex = "#ifdef USE_FOG\nvarying float vFogDepth;\nvarying vec3 vFogWorld;\n#endif";
    FOG.fog_vertex = "#ifdef USE_FOG\n\tvFogDepth = - mvPosition.z;\n\tvFogWorld = (modelMatrix * vec4(transformed, 1.0)).xyz;\n#endif";
    FOG.fog_pars_fragment = `#ifdef USE_FOG
uniform vec3 fogColor;
uniform float fogNear, fogFar;
varying float vFogDepth;
varying vec3 vFogWorld;
${RAYS > 0 ? RAY_GLSL : "uniform float uMistY;"}
#endif`;
    FOG.fog_fragment = `#ifdef USE_FOG
	{
		const float k = 1.0 / ${MIST_H.toFixed(1)};
		vec3 rd = vFogWorld - cameraPosition;
		float e0 = exp(-clamp((cameraPosition.y - uMistY) * k, -3.0, 40.0));
		float e1 = exp(-clamp((vFogWorld.y - uMistY) * k, -3.0, 40.0));
		/* mean of exp(-(y - uMistY) / MIST_H) along the ray */
		float dyk = rd.y * k;
		float mean = abs(dyk) > 1e-3 ? (e0 - e1) / dyk : e0;
		float mist = 1.0 - exp(-${(0.0016 * MIST).toFixed(5)} * length(rd) * mean);
		float fogFactor = 1.0 - (1.0 - smoothstep(fogNear, fogFar, vFogDepth)) * (1.0 - mist);
		float Y = dot(gl_FragColor.rgb, vec3(0.2126, 0.7152, 0.0722));
		gl_FragColor.rgb = mix(gl_FragColor.rgb, vec3(Y), fogFactor * 0.6);
		gl_FragColor.rgb = mix(gl_FragColor.rgb, fogColor, fogFactor);
		${RAYS > 0 ? "if (uRayHere > 0.5) gl_FragColor.rgb += quadMean(lightShafts(cameraPosition, vFogWorld));" : ""}
	}
#endif`;
    function patchFog(sh) {
        for (const k of ["vertexShader", "fragmentShader"])
            sh[k] = sh[k].replace(/^[ \t]*#include +<(fog_\w+)>/gm, (inc, name) => FOG[name] ?? inc); /* as three resolves its includes */
    }
    /* What the haze and the tilt-shift go by: the distance to the orbit
     * target, which fly mode keeps where it was (controls.js), so switching
     * modes leaves the picture as it was. Back from fly mode, where the
     * target moves to what the view meets, it eases there over LOOK_EASE ms
     * (lookFrom, lookT0) instead of jumping. */
    const LOOK_EASE = 1000;
    app.lookT0 = -1;
    function lookDist() {
        const d = camera.position.distanceTo(controls.target);
        if (app.lookT0 < 0) return d;
        const k = (performance.now() - app.lookT0) / LOOK_EASE;
        if (k >= 1) { app.lookT0 = -1; return d; }
        return app.lookFrom * Math.pow(d / app.lookFrom, k * k * (3 - 2 * k));
    }
    function fitFog() {
        const d = camera.position.distanceTo(controls.target), look = lookDist();
        /* near and far follow the distance, so a whole map fits in view
           without losing depth precision up close; flying, things come
           close whatever the target's distance */
        camera.near = THREE.MathUtils.clamp((app.flying ? 0 : d) / 400, 0.5, 8);
        camera.far = 3 * Math.max(d, look) + 2000;
        camera.updateProjectionMatrix();
        scene.fog.near = look + 150;
        scene.fog.far = look + Math.max(1000, look);
    }

    Object.assign(app, { EYE_GLSL, MIST_Y, RAYS, RAY_FAR, RAY_GLSL, fitFog, lookDist, patchFog, rayU });
}
