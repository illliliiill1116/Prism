/*
 * How the tiles' materials take the light (patchLight, patchCutout, patchWater)
 * and what they leave in the shadow maps (depthMat).
 *
 * COLOR_0 is light x tint as the 2.5D renderer applies it - a multiplier
 * on sRGB texels - at half scale, without the sun; _SUN is what a full sun
 * adds and _SUNWIN the (up to two) stretches of its arc in which far
 * blockers let it through (see src/light.c); _BLK is the block light alone,
 * of which COLOR_0 holds BLOCK_DAY: at night (the sun goes on round under
 * the map, 180-360 degrees) the rest of COLOR_0, the sky's part, dims and
 * the block light comes up to full. _BNC and _BAMP are the sun's bounce
 * off what is around (see BOUNCE). The sun term is added before the GPU's linear maths: every
 * material raises the sum to 2.2 after doubling it; there is no tone
 * mapping. The WATER material drops the texture for a clear, tinted surface
 * with a Fresnel sky reflection and a sun glint, its top rippled by wind
 * waves (?ripple= their steepness, 0 = flat; see RIPPLE).
 *
 * Shade lift (?amb=): shaded faces get AMB_LIFT of their own no-sun light
 * on top, fading out as the sun's share of the light grows, so the shaded
 * side is not so dark while lit faces stay as they were. It is a share of
 * the baked light, not a constant, so biome tint and corner darkening stay
 * as baked.
 */
import * as THREE from "three";
import { CANOPY_A } from "./tile.js";

export function setupMaterials(app) {
    const { BLOCK_DAY, MSAA, NEAR0, NEAR1, PENUMBRA, PLANT_BIAS, PLANT_CAST, SHADOW_RANGE, SHADOW_SIZE, SUN_DIR, SUN_REF,
            SUN_TILT, THIN_NEAR0, THIN_NEAR1, camera, patchFog, q, rayU, shadowU, skyU, sunU, zoomK } = app;
    const AMB_LIFT = 0.6;
    /* light plus the shade lift: weighted by dark / light, which is 1 in full
       shade and small where the sun dominates */
    const LIFT_GLSL = `vec3 shadeLift(vec3 light, vec3 dark) {
	const vec3 Y = vec3(0.2126, 0.7152, 0.0722);
	return light + ${AMB_LIFT.toFixed(2)} * dark * clamp(dot(dark, Y) / max(dot(light, Y), 1e-4), 0.0, 1.0);
}`;
    /* A light-giving block's bright texels (by luminance: a lantern's flame,
       not its frame) glow on top of the light they get, as much as the block
       light it gives off (glow, from COLOR_0's alpha) times GLOW, fading with
       the distance from the eye: half at GLOW_D blocks, a tenth at about 4.6x,
       so a far light is a dim point, not a bright one that bloom spreads. */
    const GLOW = 5;
    const GLOW_D = 64;
    const GLOW_SET = `vGlow = glow / (1.0 + pow(length(mvPosition.xyz) / ${GLOW_D.toFixed(1)}, 1.5));`;
    const GLOW_VERT = "attribute float glow;\nvarying float vGlow;";
    const GLOW_GLSL = `float glowMask(vec3 tex) { return smoothstep(0.2, 0.7, dot(tex, vec3(0.2126, 0.7152, 0.0722))); }`;
    const GLOW_ADD = `diffuseColor.rgb += tex * vGlow * ${GLOW.toFixed(2)} * glowMask(tex);`;
    /* The sun at a corner: _SUN while the sun is inside one of the corner's
       windows, eased in and out over SUN_EASE 255ths of the arc (vertex),
       times the face's diffuse (fragment, by its normal; plants as if facing
       up). Each corner switches at its own angle, so shadow edges sweep. */
    const SUN_EASE = 2.5; /* the bake keeps the angles in steps of 4 */
    /* Leaves (_SUN alpha LEAF_A) take the sun as shader packs light foliage:
       on top of the face's own diffuse, light scattered through the leaves
       (subsurface) - LEAF_SIDE of the sun whichever way the face turns, up to
       LEAF_BACK facing away from it, where it shines through - so a canopy
       does not fall apart into cubes each with a dark side. Having passed
       through a leaf the light takes on its colour once more (LEAF_TINT of
       it), which makes the lit side glow gold-green. The bake lets the sun
       through a canopy's outer leaves; their shade is the bake's alone, as
       the near map would cut it up by the leaves' holes. */
    const LEAF_A = 240 / 255;
    const LEAF_SIDE = 0.35;
    const LEAF_BACK = 0.6;
    const LEAF_TINT = 0.6;
    /* Plants (grass, flowers: lit as facing up) get the same light through
       their blades, PLANT_SSS of the sun tinted alike; the bake darkens their
       foot (PLANT_BASE), so a tuft goes from shade at the ground to a lit,
       gold-green tip. */
    const PLANT_SSS = 0.35;
    /* Wind: plants sway, their tips WAVE blocks at most (?wave=, 0 = still),
       in two waves of their own phase per spot under slow gusts that roll
       across the land. Their shadows in the map stand still. While a tile
       with plants is in view near enough that a block-high plant's tip moves
       a pixel or more (swayNear), frames keep coming, WAVE_FPS a second;
       farther off, the whole scene was redrawn for motion under a pixel. */
    const WAVE = q.has("wave") ? Number(q.get("wave")) : 0.09, WAVE_FPS = 30;
    const WAVE_GLSL = `uniform float uTime;
vec3 sway() {
	float w = blk.a * 255.0 / 128.0; /* blocks up the plant */
	if (w <= 0.0) return vec3(0.0);
	vec3 p = (modelMatrix * vec4(position, 1.0)).xyz;
	float gust = 0.55 + 0.45 * sin(uTime * 0.6 - dot(p.xz, vec2(0.045, 0.03)));
	float a = sin(uTime * 1.9 + dot(p.xz, vec2(0.61, 0.43))) + 0.4 * sin(uTime * 3.3 + dot(p.xz, vec2(-0.37, 0.83)));
	vec2 o = vec2(0.8, 0.6) * (a * gust + 0.5 * gust) * ${WAVE.toFixed(3)} * w;
	return vec3(o.x, -0.15 * abs(a) * gust * ${WAVE.toFixed(3)} * w, o.y) / length(modelMatrix[0].xyz);
}`;
    const waveU = { uTime: { value: 0 } };
    /* Ripples: the water's top tilted by a few wind waves (RIPPLE_GLSL), from
       the wind's way as the plants', each at the speed a deep-water wave of its
       length runs, stronger and weaker in drifting patches. Each fades out where its length is under RIPPLE_PX pixels:
       no shimmer far off. Frames keep coming for them (as for plants) only
       while water in view is near enough that the longest is RIPPLE_MOVE_PX
       or more (rippleNear): further off they still show, but stand still once
       the view does - looking down on the map, much of its water (aquifers,
       ponds under trees) is in the view's box but hidden, and kept the frames
       coming for nothing. */
    const RIPPLE = q.has("ripple") ? Number(q.get("ripple")) : 1, RIPPLE_PX = 12, RIPPLE_MOVE_PX = 48;
    const RIPPLES = [[5.3, 0.0, 0.05], [3.4, 0.9, 0.05], [2.3, -0.6, 0.045], [1.6, 1.8, 0.04], [1.15, -1.4, 0.035], [0.8, 0.4, 0.03]]; /* length (blocks), turn from the wind (rad), slope */
    /* the surface's slope (d/dx, d/dz) at p and its curvature (the slope's
       divergence), each wave faded by the pixel's footprint fp */
    const RIPPLE_GLSL = `uniform float uTime, uPixK;
vec3 ripple(vec2 p, float fp) {
	vec3 g = vec3(0.0);
	/* calmer and rougher patches, drifting (two slow crossed swells) */
	float gust = 0.65 + 0.55 * sin(dot(p, vec2(0.11, 0.06)) - uTime * 0.4) * sin(dot(p, vec2(-0.05, 0.13)) + uTime * 0.25);
	/* the wave fronts bent a little, slowly: straight ones crossed in a
	   lattice, the caustics in a grid of dots */
	p += vec2(sin(dot(p, vec2(0.23, -0.17)) + uTime * 0.2), sin(dot(p, vec2(0.19, 0.29)) - uTime * 0.15)) * 0.9;
	float ph;
${RIPPLES.map(([L, turn, k]) => {
        const a = Math.atan2(0.6, 0.8) + turn, w = 2 * Math.PI / L, c = Math.sqrt(9.8 * L / (2 * Math.PI)) * 0.35;
        return `	ph = dot(p, vec2(${(Math.cos(a) * w).toFixed(4)}, ${(Math.sin(a) * w).toFixed(4)})) - uTime * ${(w * c).toFixed(4)};
	g += vec3(${Math.cos(a).toFixed(4)} * cos(ph), ${Math.sin(a).toFixed(4)} * cos(ph), ${(-w).toFixed(4)} * sin(ph)) * ${(k * RIPPLE).toFixed(4)} * (1.0 - smoothstep(${(L / RIPPLE_PX / 2).toFixed(4)}, ${(L / RIPPLE_PX).toFixed(4)}, fp));`;
    }).join("\n")}
	return g * gust;
}`;
    const rippleU = { uPixK: { value: 0 } }; /* blocks per pixel per block away */
    /* Bounce: the sun as the lit faces around a corner reflect it onto it,
       baked (prism's bounce): a colour (_BNC) and how much per unit of the
       sun's height (_BAMP x, coded as there), times the sun's height and gain
       (the moon's by night). Shade picks up the colour of what is around it;
       the bake's flat skylight floor is low to leave it room. It eases off
       towards BOUNCE_CAP (b / (1 + b / cap)): a block shut in by lit faces
       (inside a crown, whatever it is built of) took enough to glow. ?bounce=
       scales it (0 none). */
    const BOUNCE = 6, BOUNCE_CAP = 0.15;
    const BOUNCE_MAX = 0.5, BNC_Q = 7, BAMP_Q = 31; /* as prism's */
    const BOUNCE_GLSL = BOUNCE > 0 ? `
	{
		/* bounce: _BAMP x in bits 0-4, _BNC rgb in 5-7, 8-10, 11-13 (see packBounce) */
		float a = mod(bb, 32.0) * ${(1 / BAMP_Q).toFixed(6)};
		vec3 bnc = mod(floor(bb / vec3(32.0, 256.0, 2048.0)), 8.0);
		float up = max(sin(uSunT * ${(Math.PI / 255).toFixed(6)}), 0.0) * ${Math.cos(SUN_TILT).toFixed(4)};
		float b = a * a * up * ${(BOUNCE_MAX * BOUNCE).toFixed(4)};
		b /= 1.0 + b / ${BOUNCE_CAP.toFixed(4)};
		vColor.rgb += bnc * (b * ${(1 / BNC_Q).toFixed(6)}) * uSunGain;
	}` : "";
    /* how much of the sun gets through the vertex's windows (_SUNWIN) now */
    const SUN_WIN_GLSL = `float sunIn(float a, float b) {
	return a > b ? 0.0 : smoothstep(a - ${SUN_EASE}, a + ${SUN_EASE}, uSunT) * (1.0 - smoothstep(b - ${SUN_EASE}, b + ${SUN_EASE}, uSunT));
}
float sunWindows(vec4 sunwin) {
	vec4 w = floor(sunwin * 255.0 + 0.5);
	/* a window reaching the horizon stays open past it */
	w.xz = mix(w.xz, vec2(-9.0), step(w.xz, vec2(0.0)));
	w.yw = mix(w.yw, vec2(264.0), step(vec2(252.0), w.yw));
	return max(sunIn(w.x, w.y), sunIn(w.z, w.w));
}`;
    const SUN_VERT = `attribute vec4 sun, sunwin, blk;
attribute float bb;
uniform float uSunT;
uniform vec3 uSunGain, uSkyCol;
uniform float uBlkGain;
${WAVE > 0 ? WAVE_GLSL : "vec3 sway() { return vec3(0.0); }"}
centroid varying vec3 vSunAdd;
varying vec3 vWPos;
varying float vPlant, vLeaf;
${SUN_WIN_GLSL}`;
    const SUN_VERT_MAIN = `{
	vColor.rgb = max(vColor.rgb - ${BLOCK_DAY.toFixed(2)} * blk.rgb, 0.0) * uSkyCol + blk.rgb * uBlkGain;${BOUNCE_GLSL}
	vSunAdd = sun.rgb * sunWindows(sunwin) * uSunGain;
	vPlant = 1.0 - sun.a;
	vLeaf = step(abs(sun.a - ${LEAF_A.toFixed(4)}), 0.02);
	vWPos = (modelMatrix * vec4(transformed, 1.0)).xyz;
}`;
    const SUN_FRAG = `centroid varying vec3 vSunAdd;
varying vec3 vWPos;
varying float vPlant, vLeaf;
uniform vec3 uSun;
float sunDiffuse() {
	vec3 N = vec3(0.0, 1.0, 0.0);
	if (vPlant < 0.5) {
		vec3 Nc = cross(dFdx(vWPos), dFdy(vWPos));
		float Nl = length(Nc);
		if (Nl < 1e-9) return 0.0;
		N = Nc / Nl;
		if (dot(N, cameraPosition - vWPos) < 0.0) N = -N;
	}
	return clamp(dot(N, uSun) / ${SUN_REF}, 0.0, 1.3);
}
/* the sun scattered through a leaf or blade: the light's own colour at
   full strength, LEAF_TINT of it - tinting only ever takes light away. A
   multisampled pixel at a triangle's edge reads its varyings past the
   edge, where they can come out below 0: the ratio made bright specks. */
vec3 throughLeaf(vec3 sa) {
	return mix(vec3(1.0), sa / max(max(sa.r, max(sa.g, sa.b)), 1e-3), ${LEAF_TINT.toFixed(3)});
}
/* what the sun adds here */
vec3 sunLight() {
	vec3 sa = max(vSunAdd, 0.0);
	if (vPlant > 0.5) return sa * (sunDiffuse() + ${(PLANT_SSS / SUN_REF).toFixed(4)} * throughLeaf(sa));
	if (vLeaf < 0.5) return vSunAdd * sunDiffuse();
	vec3 Nc = cross(dFdx(vWPos), dFdy(vWPos));
	float Nl = length(Nc), d = Nl < 1e-9 ? 0.0 : dot(Nc / Nl, uSun);
	if (dot(Nc, cameraPosition - vWPos) < 0.0) d = -d;
	float sss = ${LEAF_SIDE.toFixed(3)} + ${(LEAF_BACK - LEAF_SIDE).toFixed(3)} * max(-d, 0.0);
	return sa * (max(d, 0.0) + sss * throughLeaf(sa)) / ${SUN_REF};
}`;
    /* A pixel at a triangle's edge is shaded at its centre, which with
     * multisampling may lie outside the triangle: its corners' light is then
     * extrapolated, far past any of them on a sliver seen edge-on, and through
     * the 2.2 power one sample came out a thousand times too bright. With the
     * effects (a half-float target) that sample made the whole pixel white,
     * then bloomed: a short bright line or dot flashing as the view moved, most
     * of all far off at night, where block light differs most between corners.
     * centroid keeps the light inside the triangle. */
    function centroidColor(sh) {
        for (const k of ["vertexShader", "fragmentShader"])
            sh[k] = sh[k].replace(k === "vertexShader" ? "#include <color_pars_vertex>" : "#include <color_pars_fragment>",
                                  "#ifdef USE_COLOR\ncentroid varying vec3 vColor;\n#endif");
    }
    /* Caustics: under water, where the water map says the sun came in wd
     * blocks back along its ray, the ripples there (RIPPLE) focus it: crests
     * act as lenses, more so the deeper (1 / (1 + depth x curvature)), easing
     * back to plain light deep down. The sun's shadows down there wave with
     * them, the map read where the bent ray would have come from. Only where
     * the near shadow map reaches. */
    const CAUSTIC_GLSL = RIPPLE > 0 ? `uniform sampler2D tWater;
vec3 gShift = vec3(0.0);
float caustic() {
	/* not on plants: kelp lit up in patches read as glowing, and a kelp
	   forest's layers each paid for it */
	if (vPlant > 0.5) return 1.0;
	vec3 sw = (uShadowMat * vec4(vWPos, 1.0)).xyz * 0.5 + 0.5;
	if (max(abs(sw.x - 0.5), abs(sw.y - 0.5)) >= 0.5) return 1.0;
	float wd = (sw.z - texture2D(tWater, sw.xy).r) * ${SHADOW_RANGE}.0;
	if (wd < 0.02) return 1.0;
	vec3 r = ripple((vWPos + uSun * wd).xz, distance(cameraPosition, vWPos) * uPixK);
	float d = min(wd, 8.0);
	gShift = vec3(r.x, 0.0, r.y) * d * 0.25;
	return mix(1.0, clamp(1.0 / max(1.0 + d * 0.6 * r.z, 0.2), 0.0, 4.0), exp(-wd * 0.04));
}` : "float caustic() { return 1.0; }\nvec3 gShift = vec3(0.0);";
    function patchLight(mat, shadowed, sunlit) {
        mat.onBeforeCompile = sh => {
            centroidColor(sh);
            patchFog(sh);
            Object.assign(sh.uniforms, rayU);
            if (sunlit) Object.assign(sh.uniforms, sunU, waveU, { uSun: { value: SUN_DIR } });
            if (!shadowed) {
                if (sunlit) sh.vertexShader = sh.vertexShader
                    .replace("#include <common>", "#include <common>\n" + SUN_VERT)
                    .replace("#include <begin_vertex>", "#include <begin_vertex>\n\ttransformed += sway();")
                    .replace("#include <project_vertex>", "#include <project_vertex>\n\t" + SUN_VERT_MAIN);
                sh.fragmentShader = sh.fragmentShader.replace("#include <common>",
                    "#include <common>\n" + (sunlit ? SUN_FRAG : "vec3 sunLight() { return vec3(0.0); }") + "\n" + LIFT_GLSL)
                    .replace("#include <color_fragment>",
                    "vec3 tex = diffuseColor.rgb;\n\tvec3 dark = pow(vColor.rgb * 2.0, vec3(2.2));\n\t" +
                    "diffuseColor.rgb *= shadeLift(pow((vColor.rgb + sunLight()) * 2.0, vec3(2.2)), dark);\n\t" + GLOW_ADD);
                sh.vertexShader = sh.vertexShader.replace("#include <common>", "#include <common>\n" + GLOW_VERT)
                    .replace("#include <project_vertex>", "#include <project_vertex>\n\t" + GLOW_SET);
                sh.fragmentShader = sh.fragmentShader.replace("#include <common>", "#include <common>\nvarying float vGlow;\n" + GLOW_GLSL);
                return;
            }
            Object.assign(sh.uniforms, shadowU, rippleU);
            sh.vertexShader = sh.vertexShader
                .replace("#include <common>", "#include <common>\n" + SUN_VERT + "\n" + GLOW_VERT)
                .replace("#include <begin_vertex>", "#include <begin_vertex>\n\ttransformed += sway();")
                .replace("#include <project_vertex>", `#include <project_vertex>
	${SUN_VERT_MAIN}
	${GLOW_SET}`);
            sh.fragmentShader = sh.fragmentShader
                .replace("#include <common>", `#include <common>
${SUN_FRAG}
varying float vGlow;
${GLOW_GLSL}
uniform sampler2D tShadow, tPlant, tShadow2, tPlant2, tThin, tThin2;
uniform mat4 uShadowMat, uShadowMat2;
uniform float uShadowTexel, uShadowMix;
${LIFT_GLSL}
float nearDiff(sampler2D map, vec2 uv, float z) { return (z - texture2D(map, uv).r) * ${SHADOW_RANGE}.0; }
${RIPPLE > 0 ? RIPPLE_GLSL : "uniform float uPixK;"}
${CAUSTIC_GLSL}
float nearHit(float diff, float bias) { return step(bias, diff) * (1.0 - smoothstep(${NEAR0.toFixed(2)}, ${NEAR1.toFixed(2)}, diff)); }
/* a thin blocker's (see THIN_A) shadow is the map's to THIN_NEAR1, as the bake leaves it */
float farOf(float thin) { return mix(${NEAR1.toFixed(2)}, ${THIN_NEAR1.toFixed(2)}, thin); }
float thinHit(float diff, float bias, float thin) {
	return step(bias, diff) * (1.0 - smoothstep(mix(${NEAR0.toFixed(2)}, ${THIN_NEAR0.toFixed(2)}, thin), farOf(thin), diff));
}
/* the solid map's test at one texel centre c; solid stays 1 while
   every texel tested has a blocker in reach (the search's deep shadow) */
float penHit(sampler2D tShadow, vec3 s, vec2 slope, vec2 c, float bias, float thin, inout float solid) {
	float diff = nearDiff(tShadow, c, s.z + dot(slope, c - s.xy));
	solid *= step(bias, diff) * step(diff, farOf(thin));
	return thinHit(diff, bias, thin);
}
/* one tap of the penumbra filter at map offset o: the four solid texels
   around, blended by where o falls between them, so the result moves
   smoothly with the pixel rather than in texel steps */
float penTap(sampler2D tShadow, sampler2D tPlant, vec3 s, vec2 slope, vec2 o, float bias, float thin, inout float solid) {
	vec2 t = (s.xy + o) * ${SHADOW_SIZE}.0 - 0.5, f = fract(t), c = (floor(t) + 0.5) / ${SHADOW_SIZE}.0;
	const float d = 1.0 / ${SHADOW_SIZE}.0;
	float k = mix(mix(penHit(tShadow, s, slope, c, bias, thin, solid), penHit(tShadow, s, slope, c + vec2(d, 0.0), bias, thin, solid), f.x),
	              mix(penHit(tShadow, s, slope, c + vec2(0.0, d), bias, thin, solid), penHit(tShadow, s, slope, c + vec2(d), bias, thin, solid), f.x), f.y);
	/* plants take shade from solid blockers only, not from each other;
	   a blade and a block over the same spot shade it as the block. Their
	   shadow is light: one texel will do. */
	if (vPlant < 0.5) k = max(k, ${PLANT_CAST.toFixed(2)} * nearHit(nearDiff(tPlant, s.xy + o, s.z + dot(slope, o)), bias));
	return k;
}
/* 0..1: how much a blocker that starts less than ${NEAR1} blocks towards the sun hides it, by one map pair */
float nearShadowIn(sampler2D tShadow, sampler2D tPlant, sampler2D tThin, mat4 uShadowMat) {
	vec3 Nc = cross(dFdx(vWPos), dFdy(vWPos));
	float Nl = length(Nc);
	if (Nl < 1e-9) return 0.0;
	vec3 N = Nc / Nl;
	if (dot(N, uSun) < 0.0) N = -N;
	/* A plant is not in the solid map, so it has no face of its own to keep
	   clear of: no plane for it, a step towards the sun and a small bias
	   (PLANT_BIAS) for the ground at its foot. Swaying bent each blade's
	   quad, its two triangles took planes of their own that ran into the
	   ground under it, and the shade on grass flickered in triangles. */
	if (vPlant > 0.5) N = uSun;
	vec3 s = (uShadowMat * vec4(vWPos + N * uShadowTexel * 1.5, 1.0)).xyz * 0.5 + 0.5;
	float edge = max(abs(s.x - 0.5), abs(s.y - 0.5)) * 2.0;
	if (edge >= 1.0) return 0.0;
	/* a lit face meets itself in the map: compare each tap with this
	   face's own plane there (its depth slope across the map), plus a
	   little */
	vec3 sx = dFdx(s), sy = dFdy(s);
	float det = sx.x * sy.y - sx.y * sy.x;
	vec2 slope = vPlant < 0.5 && abs(det) > 1e-14 ? vec2(sx.z * sy.y - sx.y * sy.z, sx.x * sy.z - sx.z * sy.x) / det : vec2(0.0);
	float bias = vPlant > 0.5 ? ${PLANT_BIAS.toFixed(2)} : 0.01 + uShadowTexel * 0.5;
	s.xy += (mat3(uShadowMat) * gShift).xy * 0.5; /* under rippling water (caustic) */
	float texelUv = 1.0 / ${SHADOW_SIZE}.0;
	float perBlock = 1.0 / (uShadowTexel * ${SHADOW_SIZE}.0); /* uv per block */
	/* spirals of taps the same for every pixel: turned per pixel, their
	   steps read as grain wherever a penumbra spans a few pixels */
	/* the mean distance to the blockers around sets the penumbra, never
	   wider than the search: the widest, a thin one THIN_NEAR1 blocks off */
	float penR = (0.01 + ${THIN_NEAR1.toFixed(1)} * ${PENUMBRA}) * perBlock, searchR = max(penR, 1.5 * texelUv);
	float fade = 1.0 - smoothstep(0.85, 1.0, edge);
	/* Where the widest penumbra is narrower than a pixel or a texel or
	   so (the map is spread wide when the view takes in much), the search
	   can find nothing the filter does not: four taps across the larger
	   of the two do, rather than the search and its eight. Seen from high
	   up each block is a few pixels, its triangles shade in 2x2 blocks
	   of them whatever the resolution, and the filter was three quarters
	   of the frame. Both are taken over a band, so no line shows. */
	float foot = distance(cameraPosition, vWPos) * uPixK * perBlock; /* map uv per pixel */
	float far = smoothstep(0.5, 1.0, max(foot, 1.5 * texelUv) / penR), farK = 0.0;
	if (far > 0.0) {
		float r = max(0.35 * foot, 0.7 * texelUv);
		vec2 o0 = vec2(r, 0.5 * r), o1 = vec2(-0.5 * r, r);
		/* thin (THIN_A) if any tap's nearest blocker is: those shade
		   twice as far, the canopy over grass most of all */
		float th = step(0.5, max(max(textureLod(tThin, s.xy + o0, 0.0).r, textureLod(tThin, s.xy + o1, 0.0).r),
		                         max(textureLod(tThin, s.xy - o0, 0.0).r, textureLod(tThin, s.xy - o1, 0.0).r)));
		float solid = 1.0;
		farK = 0.25 * (penTap(tShadow, tPlant, s, slope, o0, bias, th, solid) + penTap(tShadow, tPlant, s, slope, o1, bias, th, solid) +
		               penTap(tShadow, tPlant, s, slope, -o0, bias, th, solid) + penTap(tShadow, tPlant, s, slope, -o1, bias, th, solid));
		farK = max(farK, solid);
		if (far >= 1.0) return farK * fade;
	}
	float dsum = 0.0, nb = 0.0;
	/* whether a blocker found is thin; the taps below take it from here */
	float thin = 0.0, nsolid = 0.0;
	for (int i = 0; i < 12; i++) {
		float a = float(i) * 2.39996, r = sqrt((float(i) + 0.5) / 12.0);
		vec2 o = vec2(cos(a), sin(a)) * r * searchR;
		float z = s.z + dot(slope, o);
		/* whether it is thin, only where something is */
		float diff = nearDiff(tShadow, s.xy + o, z), th = 0.0;
		if (diff > bias && diff < ${THIN_NEAR1.toFixed(2)}) th = step(0.5, textureLod(tThin, s.xy + o, 0.0).r); /* filtered: half at a rim */
		if (diff > bias && diff < farOf(th)) { dsum += diff; nb += 1.0; nsolid += 1.0; thin = max(thin, th); }
		diff = vPlant < 0.5 ? nearDiff(tPlant, s.xy + o, z) : 0.0;
		if (diff > bias && diff < ${NEAR1.toFixed(2)}) { dsum += diff; nb += 1.0; }
	}
	float k = 0.0;
	/* every tap blocked by something solid: deep in the shadow */
	if (nsolid == 12.0) k = 1.0;
	else if (nb > 0.0) {
		float pen = clamp((0.01 + dsum / nb * ${PENUMBRA}) * perBlock, texelUv, searchR);
		for (int i = 0; i < 8; i++) {
			float a = float(i) * 2.39996, r = sqrt((float(i) + 0.5) / 8.0);
			float solid = 1.0;
			k += penTap(tShadow, tPlant, s, slope, vec2(cos(a), sin(a)) * r * pen, bias, thin, solid);
		}
		k /= 8.0;
	}
	return mix(k, farK, far) * fade;
}
float nearShadow() {
	float a = nearShadowIn(tShadow, tPlant, tThin, uShadowMat);
	return uShadowMix > 0.0 ? mix(a, nearShadowIn(tShadow2, tPlant2, tThin2, uShadowMat2), uShadowMix) : a;
}`)
                .replace("#include <color_fragment>", `vec3 dark = pow(vColor.rgb * 2.0, vec3(2.2));
	vec3 sl = sunLight();
	if (dot(sl, vec3(1.0)) > 0.0) sl *= caustic();
	vec3 lit = dot(sl, vec3(1.0)) > 0.0 ? pow((vColor.rgb + sl) * 2.0, vec3(2.2)) : dark;
	if (lit != dark && vLeaf < 0.5) lit = mix(lit, dark, nearShadow());
	lit = shadeLift(lit, dark);
	vec3 tex = diffuseColor.rgb;
	diffuseColor.rgb *= lit;
	${GLOW_ADD}`);
        };
    }

    /* Cut-outs (plants, leaves) as anti-aliased coverage rather than a hard
     * alpha test, which made thin blades flicker as the view turned: mip
     * levels average a blade's alpha below the cut-off, more so the more
     * edge-on it is seen, so whole texels dropped in and out. Alpha is raised
     * with the mip level to keep the coverage the texture has, then sharpened
     * to one pixel's width around the cut-off: crisp up close, a smooth
     * coverage far off. Needs multisampling; without it, the plain test.
     *
     * Plants (_SUN alpha 0) also fade out with distance, gone by
     * PLANT_FADE[1]: past LOD_D[0] a tile may be swapped for its level without
     * plants, and a whole tile's grass popping in or out as the view turned
     * read as flicker too. */
    const CUT_MIP_SCALE = 0.25;
    const PLANT_FADE = [125, 180];
    const PLANT_FADE_GLSL = `uniform float uZoomK;
bool plantFades(float a) { return a < ${(CANOPY_A / 2 / 255).toFixed(4)}; }
float plantFade(vec3 p) { return 1.0 - smoothstep(${PLANT_FADE[0].toFixed(1)}, ${PLANT_FADE[1].toFixed(1)}, distance(p, uEye) * uZoomK); }`;
    function patchCutout(mat, plants) {
        const prev = mat.onBeforeCompile;
        mat.onBeforeCompile = (sh, r) => {
            if (prev) prev(sh, r);
            if (plants) {
                sh.uniforms.uEye = { value: camera.position };
                sh.uniforms.uZoomK = zoomK;
                if (!/attribute vec4 sun\b/.test(sh.vertexShader))
                    sh.vertexShader = sh.vertexShader.replace("#include <common>", "#include <common>\nattribute vec4 sun;");
                sh.vertexShader = sh.vertexShader
                    .replace("#include <common>", `#include <common>\nuniform vec3 uEye;\nvarying float vCutFade;\n${PLANT_FADE_GLSL}`)
                    .replace("#include <project_vertex>", `#include <project_vertex>
	vCutFade = plantFades(sun.a) ? plantFade((modelMatrix * vec4(transformed, 1.0)).xyz) : 1.0;`);
                sh.fragmentShader = sh.fragmentShader.replace("#include <common>", "#include <common>\nvarying float vCutFade;");
            }
            sh.fragmentShader = sh.fragmentShader.replace("#include <alphatest_fragment>", `{
	vec2 tx = vMapUv * vec2(textureSize(map, 0)), dx = dFdx(tx), dy = dFdy(tx);
	float a = diffuseColor.a * (1.0 + max(0.0, 0.5 * log2(max(dot(dx, dx), dot(dy, dy)))) * ${CUT_MIP_SCALE});
	diffuseColor.a = clamp((a - alphaTest) / max(fwidth(a), 1e-4) + 0.5, 0.0, 1.0)${plants ? " * vCutFade" : ""};
	if (diffuseColor.a <= 0.0) discard;
}`);
        };
        mat.alphaToCoverage = true;
    }

    /* With the effects, the scene is drawn in two parts (SplitRenderPass): all
     * but water and glass, then those over it, and water reads the first
     * part's colour and depth (WATER_SCENE_GLSL): what is under it, bent by
     * the ripples and dimmed by the water it is seen through (refraction),
     * and what it mirrors, found by marching the reflected ray through the
     * depth (screen-space reflection; where the ray leaves the screen or
     * finds nothing, the sky). Without the effects it is blended over the
     * scene as before. The sun's (moon's) glint is a sharp core in a softer
     * lobe, so the ripples break it into a glittering path, wherever the
     * bake's windows let the sun reach the water. */
    const WATER_ABS = [0.11, 0.05, 0.04]; /* per block of water looked through */
    const SSR_STEPS = 24;
    const waterU = { tScene: { value: null }, tSceneDepth: { value: null }, uSceneOn: { value: 0 },
                     uProj: { value: camera.projectionMatrix },
                     tFull: { value: null }, tFullZ: { value: null }, uFullOn: { value: 0 }, uProjFull: { value: new THREE.Matrix4() } };
    /* For a large photo's tiles the reflections march through the whole view
     * as drawn once at screen size (shotGrab), not the tile: what a tile's
     * water reflects is mostly above it, out of the tile. */
    const WATER_SCENE_GLSL = `uniform sampler2D tScene, tSceneDepth, tFull, tFullZ;
uniform float uSceneOn, uFullOn;
uniform mat4 uProj, uProjFull;
float sceneZ(vec2 uv) { /* view-space z of what the first part drew there */
	float d = textureLod(tSceneDepth, uv, 0.0).r;
	return uProj[3][2] / (1.0 - 2.0 * d - uProj[2][2]);
}
vec3 toScreen(vec3 v) { vec4 c = uProj * vec4(v, 1.0); return vec3(c.xy / c.w * 0.5 + 0.5, c.w); }
vec3 marchScreen(vec3 v) { vec4 c = (uFullOn > 0.5 ? uProjFull : uProj) * vec4(v, 1.0); return vec3(c.xy / c.w * 0.5 + 0.5, c.w); }
float marchZ(vec2 uv) { return uFullOn > 0.5 ? textureLod(tFullZ, uv, 0.0).r : sceneZ(uv); }
/* the reflected ray from view-space P along R: its colour, and how much to
   trust it (fading at the screen's edges); steps growing with distance */
vec4 reflectScene(vec3 P, vec3 R) {
	float dt = 0.3 + 0.012 * -P.z, t = 0.0;
	for (int i = 0; i < ${SSR_STEPS}; i++) {
		t += dt;
		vec3 Q = P + R * t, s = marchScreen(Q);
		if (Q.z > -0.1 || any(lessThan(s.xy, vec2(0.0))) || any(greaterThan(s.xy, vec2(1.0)))) break;
		float dz = marchZ(s.xy) - Q.z;
		if (dz > 0.0 && dz < dt * 1.5 + 0.3) {
			/* passed behind something: halve back to where */
			float a = t - dt, b = t;
			for (int j = 0; j < 5; j++) {
				float m = 0.5 * (a + b);
				vec3 M = P + R * m;
				if (marchZ(marchScreen(M).xy) - M.z > 0.0) b = m; else a = m;
			}
			s = marchScreen(P + R * b);
			vec2 e = min(s.xy, 1.0 - s.xy);
			vec3 col = uFullOn > 0.5 ? textureLod(tFull, s.xy, 0.0).rgb : textureLod(tScene, s.xy, 0.0).rgb;
			return vec4(col, smoothstep(0.0, 0.08, min(e.x, e.y)));
		}
		dt *= 1.15;
	}
	return vec4(0.0);
}`;
    function patchWater(mat) {
        /* a waterlogged block's water lies in the planes of its own faces (a
           stair's side, the post above): pushed back, the solid face wins there
           instead of the two trading pixels as the view moves */
        mat.polygonOffset = true;
        mat.polygonOffsetFactor = 1;
        mat.polygonOffsetUnits = 4;
        mat.onBeforeCompile = sh => {
            centroidColor(sh);
            patchFog(sh);
            Object.assign(sh.uniforms, rayU, waterU);
            sh.uniforms.uZenith = skyU.uReflHigh;
            sh.uniforms.uHorizon = skyU.uReflLow;
            sh.uniforms.uSkyCol = sunU.uSkyCol;
            sh.uniforms.uBlkGain = sunU.uBlkGain;
            sh.uniforms.uSunT = sunU.uSunT;
            sh.uniforms.uSunGain = sunU.uSunGain;
            sh.uniforms.uSun = { value: SUN_DIR };
            sh.uniforms.uGlint = sunU.uGlint;
            sh.uniforms.uLinBlend = linBlend;
            sh.uniforms.uTime = waveU.uTime;
            sh.uniforms.uPixK = rippleU.uPixK;
            sh.vertexShader = sh.vertexShader
                .replace("#include <common>", `#include <common>
varying vec3 vWPos;
varying float vSunVis;
attribute vec4 blk, sunwin;
uniform vec3 uSkyCol;
uniform float uBlkGain, uSunT;
${SUN_WIN_GLSL}`)
                .replace("#include <project_vertex>", `#include <project_vertex>
	vWPos = (modelMatrix * vec4(transformed, 1.0)).xyz;
	vSunVis = sunWindows(sunwin);
	vColor.rgb = max(vColor.rgb - ${BLOCK_DAY.toFixed(2)} * blk.rgb, 0.0) * uSkyCol + blk.rgb * uBlkGain;`);
            sh.fragmentShader = sh.fragmentShader
                .replace("#include <common>", `#include <common>
varying vec3 vWPos;
varying float vSunVis;
uniform vec3 uZenith, uHorizon, uSun, uSunGain;
uniform float uLinBlend, uGlint;
${RIPPLE > 0 ? RIPPLE_GLSL : ""}
${WATER_SCENE_GLSL}`)
                /* no texture: the vanilla water texture's pattern read as fake ripples */
                .replace("#include <map_fragment>", "")
                .replace("#include <color_fragment>", `diffuseColor.rgb = pow(vColor.rgb * 2.0, vec3(2.2)) * 0.6;
	vec3 V = normalize(cameraPosition - vWPos);
	/* an edge-on or sub-pixel triangle has parallel derivatives: no normal */
	vec3 Nc = cross(dFdx(vWPos), dFdy(vWPos));
	float Nl = length(Nc);
	vec3 N = Nl > 1e-6 ? Nc / Nl : vec3(0.0, 1.0, 0.0);
	/* face the viewer; flipping on N.y made side faces flip per pixel */
	if (dot(N, V) < 0.0) N = -N;
	vec3 N0 = N;
${RIPPLE > 0 ? `	/* the top (flowing water's slopes too), seen from above: rippled; a
	   pixel spans the more of it the more it is seen edge on */
	if (N.y > 0.5) {
		float fp = max(distance(cameraPosition, vWPos) * uPixK, length(fwidth(vWPos.xz)) * 0.5);
		vec2 g = ripple(vWPos.xz, fp).xy;
		N = normalize(N - vec3(g.x, 0.0, g.y) * N.y);
		if (dot(N, V) < 0.02) N = normalize(N + V * (0.02 - dot(N, V)));
	}
` : ""}	/* clear water: mostly see-through looking down, a mirror at grazing angles */
	float cosT = clamp(dot(N, V), 0.0, 1.0);
	float F = 0.02 + 0.98 * pow(1.0 - cosT, 5.0);
	vec3 R = reflect(-V, N);
	/* a reflection pointing into the water (a ripple's back seen edge on) mirrors the horizon */
	vec3 sky = mix(uHorizon, uZenith, clamp(R.y * 1.2, 0.0, 1.0));
	float c = max(dot(R, uSun), 0.0);
	vec3 glint = uSunGain * (min(pow(c, 4000.0) * 120.0, 24.0) + pow(c, 500.0) * 0.3) * (0.25 + F) * uGlint * vSunVis;
	if (uSceneOn > 0.5) {
		vec2 uv = gl_FragCoord.xy / vec2(textureSize(tSceneDepth, 0));
		vec3 P = (viewMatrix * vec4(vWPos, 1.0)).xyz;
		/* under it: bent by the ripples, more the deeper; not where the
		   bent ray lands on something in front of the water */
		float deep = max(P.z - sceneZ(uv), 0.0);
		vec3 Nv = mat3(viewMatrix) * (N - N0);
		vec2 uvB = uv + Nv.xy * min(deep, 3.0) * 0.25 / max(-P.z, 1.0) * uProj[1][1];
		float zB = sceneZ(uvB);
		if (zB > P.z) { uvB = uv; zB = sceneZ(uv); }
		float path = max(P.z - zB, 0.0) * length(P) / max(-P.z, 1e-3);
		vec3 tr = exp(-path * vec3(${WATER_ABS.map(x => x.toFixed(3)).join(", ")}));
		vec3 under = textureLod(tScene, uvB, 0.0).rgb * tr + diffuseColor.rgb * (1.0 - tr) * 0.5;
		/* looking down, the mirror is a few percent: not worth the march */
		vec4 r = F > 0.03 ? reflectScene(P, mat3(viewMatrix) * R) : vec4(0.0);
		vec3 mirror = mix(sky, r.rgb, r.a * smoothstep(0.03, 0.06, F));
		diffuseColor = vec4(mix(under, mirror, F) + glint, 1.0);
	} else {
		diffuseColor.rgb = mix(diffuseColor.rgb, sky, F) + glint;
		diffuseColor.a = clamp(mix(0.28, 1.0, F) + dot(glint, vec3(0.33)), 0.0, 1.0);
		/* the effects blend in a linear target, where the same alpha looks much
		   thicker than on the sRGB canvas: thin it to match */
		diffuseColor.a = mix(diffuseColor.a, pow(diffuseColor.a, 2.2), uLinBlend);
	}`);
        };
        /* colour blends as usual; with effects on, the target's alpha is set to
         * 0 where water is in front so the grade can leave water alone. Without
         * them alpha is kept, or the canvas turns see-through there. */
        mat.blending = THREE.CustomBlending;
        mat.blendSrc = THREE.SrcAlphaFactor;
        mat.blendDst = THREE.OneMinusSrcAlphaFactor;
        mat.blendSrcAlpha = THREE.ZeroFactor;
        waterMats.push(mat);
    }
    const waterMats = [];
    const linBlend = { value: 0 }; /* 1 while drawing through the effects */

    /* What each material leaves in the [solid, plant, water] shadow maps: solid
     * geometry its sunward faces, cut-outs both sides past the alpha test
     * (plants, marked by _SUN alpha 0, in the plant map, the rest in the
     * solid one), water its surface in the water map, glass nothing. The
     * solid map's colour is 1 where
     * the nearest blocker is thin (_SUN alpha THIN_A: a fence, a post), whose
     * shadow the map draws further, to THIN_NEAR1; past that the bake takes
     * it (prism's far_occl). */
    const THIN_A = 191 / 255;
    const THIN_GLSL = `float thinOf(float a) { return a > 0.5 && a < 0.9 ? 1.0 : 0.0; }`;
    const depthMats = new Map();
    function solidDepth() {
        return new THREE.ShaderMaterial({
            vertexShader: `attribute vec4 sun;
varying float vThin;
${THIN_GLSL}
void main() {
	vThin = thinOf(sun.a);
	gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
}`,
            fragmentShader: `varying float vThin;
void main() { gl_FragColor = vec4(vThin, 0.0, 0.0, 1.0); }`,
        });
    }
    function cutoutDepth(m, plants) {
        return new THREE.ShaderMaterial({
            uniforms: { map: { value: m.map }, uEye: { value: camera.position }, uZoomK: zoomK },
            side: THREE.DoubleSide,
            vertexShader: `attribute vec4 sun;
uniform vec3 uEye;
varying vec2 vUv;
varying float vKeep, vThin, vLeaf;
${PLANT_FADE_GLSL}
${THIN_GLSL}
void main() {
	vUv = uv;
	vThin = thinOf(sun.a);
	/* plants that have faded from view (see patchCutout) cast nothing */
	vKeep = (sun.a < 0.5) == ${plants} ? 1.0 : 0.0;
	vLeaf = step(abs(sun.a - ${LEAF_A.toFixed(4)}), 0.02);
	if (${plants && MSAA} && plantFades(sun.a)) vKeep *= step(0.5, plantFade((modelMatrix * vec4(position, 1.0)).xyz));
	gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
}`,
            fragmentShader: `uniform sampler2D map;
varying vec2 vUv;
varying float vKeep, vThin, vLeaf;
void main() {
	/* leaves cast as whole blocks, as the bake takes them: through their
	   texture's holes a sparse one (cherry) cast a scatter of dots */
	if (vKeep < 0.5 || vLeaf < 0.5 && texture2D(map, vUv).a < 0.5) discard;
	gl_FragColor = vec4(vThin, 0.0, 0.0, 1.0);
}`,
        });
    }
    function depthMat(m) {
        if (m.name === "WATER") return [null, null, waterDepthMat];
        if (m.name !== "OPAQUE" && m.name !== "MASK") return [null, null, null];
        if (!depthMats.has(m)) depthMats.set(m, m.name === "OPAQUE"
            ? [solidDepth(), null, null]
            : [cutoutDepth(m, false), cutoutDepth(m, true), null]);
        return depthMats.get(m);
    }
    /* the water map: its surface, either side */
    const waterDepthMat = new THREE.ShaderMaterial({
        side: THREE.DoubleSide,
        vertexShader: "void main() { gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }",
        fragmentShader: "void main() { gl_FragColor = vec4(0.0); }",
    });

    Object.assign(app, { PLANT_FADE, RIPPLE, RIPPLES, RIPPLE_MOVE_PX, WAVE, WAVE_FPS, depthMat, depthMats, linBlend, patchCutout,
                       patchLight, patchWater, rippleU, waterMats, waterU, waveU });
}
