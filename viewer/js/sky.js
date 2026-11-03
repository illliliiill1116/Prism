/*
 * The sky dome: the atmosphere's colour, the sun's disc, moon and stars.
 *
 * Sky: a dome lit by single scattering in a model atmosphere (Rayleigh
 * air, Mie haze), the sun in it; fog and water take their colours from it
 * (?exposure= its brightness). At night a procedural star field with the
 * real sky's brightness and colour spread, twinkling, dimmed and reddened
 * near the horizon, and the Milky Way; it turns with the moon (?stars=
 * brightness, 0 = none). Distance haze plus ground mist low down
 * (?mist=, 0 = none), both greying colour before it fades to the horizon.
 */
import * as THREE from "three";

export function setupSky(app) {
    const { NIGHT_HORIZON, NIGHT_REFL_HIGH, NIGHT_REFL_LOW, NIGHT_ZENITH, RAYS, RAY_FAR, RAY_GLSL, SKY_SUN, SUN_COL,
            SUN_TILT, q, rayU, scene, skyU } = app;
    /* The sky's colour by single scattering (Nishita): along the view ray,
     * sunlight that reached each point through the air, scattered towards the
     * eye by air molecules (Rayleigh: blue, broad) and haze (Mie: white, bunched
     * around the sun), both thinned on the way. Low suns light through more
     * air, so sunsets redden by themselves and the sky darkens after them.
     * Worked out on the CPU into a LUT whenever the sun moves - the sky is
     * symmetric about the sun's vertical plane, so by the angle from the sun's
     * bearing (0..180) and elevation (0..90, sqrt-spaced: detail at the horizon)
     * - and tone mapped there (1 - e^-x), so what the fog and water take from
     * it is exactly what the dome shows. */
    const ATM = { R: 6360e3, RA: 6420e3, HR: 8e3, HM: 1.2e3, EYE: 600,
                  BR: [5.8e-6, 13.5e-6, 33.1e-6], BM: 21e-6, G: 0.76, SUN: 20,
                  /* ozone: absorbs, scatters nothing, in a layer around 25 km; it
                     takes the yellow-green out of long low paths (no olive horizon) */
                  BO: [0.65e-6, 1.881e-6, 0.085e-6], OH: 25e3, OW: 15e3 };
    const ozone = h => Math.max(0, 1 - Math.abs(h - ATM.OH) / ATM.OW);
    /* extinction of channel c over air (Rayleigh, Mie, ozone) */
    const ext = (c, r, m, o) => ATM.BR[c] * r + ATM.BM * 1.1 * m + ATM.BO[c] * o;
    const EXPOSURE = 1.6;
    const SKY_W = 48, SKY_H = 32;
    const skyData = new Uint16Array(SKY_W * SKY_H * 4);
    const skyLut = new THREE.DataTexture(skyData, SKY_W, SKY_H, THREE.RGBAFormat, THREE.HalfFloatType);
    skyLut.magFilter = skyLut.minFilter = THREE.LinearFilter;
    skyLut.wrapS = skyLut.wrapT = THREE.ClampToEdgeWrapping;
    /* how far along (ox, oy, oz) + t (dx, dy, dz) the ray leaves the sphere of radius r; < 0 if it misses */
    function sphereExit(ox, oy, oz, dx, dy, dz, r) {
        const b = ox * dx + oy * dy + oz * dz, c = ox * ox + oy * oy + oz * oz - r * r, h = b * b - c;
        return h < 0 ? -1 : -b + Math.sqrt(h);
    }
    /* air (Rayleigh, Mie, ozone) between a point and the sun, or null in the earth's shadow */
    const odL = [0, 0, 0];
    function sunDepth(px, py, pz, sx, sy, sz) {
        const b = px * sx + py * sy + pz * sz, c = px * px + py * py + pz * pz - ATM.R * ATM.R;
        if (b < 0 && b * b - c > 0) return null;
        const t = sphereExit(px, py, pz, sx, sy, sz, ATM.RA), n = 6, ds = t / n;
        odL[0] = odL[1] = odL[2] = 0;
        for (let i = 0; i < n; i++) {
            const u = (i + 0.5) * ds, x = px + sx * u, y = py + sy * u, z = pz + sz * u;
            const h = Math.sqrt(x * x + y * y + z * z) - ATM.R;
            odL[0] += Math.exp(-h / ATM.HR) * ds; odL[1] += Math.exp(-h / ATM.HM) * ds; odL[2] += ozone(h) * ds;
        }
        return odL;
    }
    /* the light scattered to the eye from direction d, sun s (unit vectors), before
     * tone mapping. Steps grow with distance (t ~ i^2): along the horizon blue is
     * used up within a few tens of km, and a first step that long would dim its
     * own light as though it were all at the far end - an olive horizon. */
    function scatter(dx, dy, dz, sx, sy, sz, out) {
        const oy = ATM.R + ATM.EYE, n = 16;
        const t = sphereExit(0, oy, 0, dx, dy, dz, ATM.RA);
        let odR = 0, odM = 0, odO = 0;
        const sr = [0, 0, 0], sm = [0, 0, 0];
        for (let i = 0; i < n; i++) {
            const t0 = t * (i / n) ** 2, t1 = t * ((i + 1) / n) ** 2, ds = t1 - t0, u = (t0 + t1) / 2;
            const x = dx * u, y = oy + dy * u, z = dz * u;
            const h = Math.sqrt(x * x + y * y + z * z) - ATM.R;
            const hr = Math.exp(-h / ATM.HR) * ds, hm = Math.exp(-h / ATM.HM) * ds, ho = ozone(h) * ds;
            /* the air up to the middle of this step */
            const l = sunDepth(x, y, z, sx, sy, sz);
            if (l)
                for (let c = 0; c < 3; c++) {
                    const a = Math.exp(-ext(c, odR + hr / 2 + l[0], odM + hm / 2 + l[1], odO + ho / 2 + l[2]));
                    sr[c] += a * hr; sm[c] += a * hm;
                }
            odR += hr; odM += hm; odO += ho;
        }
        const mu = dx * sx + dy * sy + dz * sz, g = ATM.G;
        const pr = 3 / (16 * Math.PI) * (1 + mu * mu);
        const pm = 3 / (8 * Math.PI) * (1 - g * g) * (1 + mu * mu) / ((2 + g * g) * Math.pow(1 + g * g - 2 * g * mu, 1.5));
        for (let c = 0; c < 3; c++) out[c] = ATM.SUN * (sr[c] * ATM.BR[c] * pr + sm[c] * ATM.BM * pm);
        return out;
    }
    /* the sun's light after the air between it and the eye */
    function sunThrough(sx, sy, sz, out) {
        const l = sunDepth(0, ATM.R + ATM.EYE, 0, sx, sy, sz);
        for (let c = 0; c < 3; c++) out[c] = l ? Math.exp(-ext(c, l[0], l[1], l[2])) : 0;
        return out;
    }
    const tone = x => 1 - Math.exp(-EXPOSURE * x);
    /* the LUT's elevation for row j, and the row for an elevation (as the shader's) */
    const lutEl = j => (j / (SKY_H - 1)) ** 2 * Math.PI / 2;
    const lutAvg = (el, out) => { /* the ring at elevation el, averaged around: fog and water */
        const j = Math.sqrt(el / (Math.PI / 2)) * (SKY_H - 1), j0 = Math.floor(j), j1 = Math.min(j0 + 1, SKY_H - 1), f = j - j0;
        out[0] = out[1] = out[2] = 0;
        for (let i = 0; i < SKY_W; i++)
            for (let c = 0; c < 3; c++) {
                const w = (i == 0 || i == SKY_W - 1) ? 0.5 : 1;
                out[c] += w * (skyRaw[(j0 * SKY_W + i) * 3 + c] * (1 - f) + skyRaw[(j1 * SKY_W + i) * 3 + c] * f) / (SKY_W - 1);
            }
        return out;
    };
    const skyRaw = new Float32Array(SKY_W * SKY_H * 3);
    const atmTmp = [0, 0, 0], atmT2 = [0, 0, 0];
    let atmKey = null;
    function atmosphere(n) {
        /* in the LUT's frame: the sun's bearing along +x */
        const se = Math.asin(THREE.MathUtils.clamp(SKY_SUN.y, -1, 1)), sx = Math.cos(se), sy = Math.sin(se);
        const key = se.toFixed(5) + "/" + n.toFixed(4);
        if (key == atmKey) return;
        atmKey = key;
        for (let j = 0; j < SKY_H; j++) {
            const el = lutEl(j), ce = Math.cos(el), dy = Math.sin(el);
            for (let i = 0; i < SKY_W; i++) {
                const az = i / (SKY_W - 1) * Math.PI;
                scatter(ce * Math.cos(az), dy, ce * Math.sin(az), sx, sy, 0, atmTmp);
                for (let c = 0; c < 3; c++) {
                    const v = tone(atmTmp[c]);
                    skyRaw[(j * SKY_W + i) * 3 + c] = v;
                    skyData[(j * SKY_W + i) * 4 + c] = THREE.DataUtils.toHalfFloat(v);
                }
                skyData[(j * SKY_W + i) * 4 + 3] = THREE.DataUtils.toHalfFloat(1);
            }
        }
        skyLut.needsUpdate = true;
        /* what the dome is mixed towards below the horizon, the fog, and the water's reflection */
        const add = (src, night, out) => out.setRGB(src[0], src[1], src[2]).add(night.clone().convertSRGBToLinear().multiplyScalar(n));
        add(lutAvg(0.02, atmT2), NIGHT_HORIZON, skyU.uHorizon.value);
        add(lutAvg(Math.PI / 2, atmT2), NIGHT_ZENITH, skyU.uZenith.value);
        add(lutAvg(0.6, atmT2), NIGHT_REFL_HIGH, skyU.uReflHigh.value);
        add(lutAvg(0.08, atmT2), NIGHT_REFL_LOW, skyU.uReflLow.value);
        if (scene.fog) scene.fog.color.copy(skyU.uHorizon.value);
        /* the disc: white sunlight through the air */
        sunThrough(SKY_SUN.x, SKY_SUN.y, SKY_SUN.z, atmT2);
        const m = Math.max(atmT2[0], atmT2[1], atmT2[2], 1e-6);
        SUN_COL.setRGB(atmT2[0] / m, atmT2[1] / m, atmT2[2] / m).multiplyScalar(Math.min(1, m * 4));
    }
    /* The night sky turns with the sun and moon: about the axis square to their
     * arc (sunAt's plane), by the sun's angle, so stars rise in the east. */
    const STAR_AXIS = new THREE.Vector3(0, Math.sin(SUN_TILT), Math.cos(SUN_TILT));
    const starRot = new THREE.Matrix3(), starRot4 = new THREE.Matrix4();
    const STARS = q.has("stars") ? Number(q.get("stars")) : 1;
    /* The Milky Way placed as it should look at midnight (sun at 270): arching
     * high across the sky, its bright centre some 25 degrees up; turned back by
     * that hour into the sky's own frame. */
    const GAL = (() => {
        const pole = new THREE.Vector3(0.6, 0.15, 0.78).normalize(), up = new THREE.Vector3(0, 1, 0);
        const across = new THREE.Vector3().crossVectors(pole, up).normalize();
        const rise = up.clone().addScaledVector(pole, -pole.dot(up)).normalize();
        const centre = across.multiplyScalar(Math.cos(0.44)).addScaledVector(rise, Math.sin(0.44));
        const back = new THREE.Matrix4().makeRotationAxis(STAR_AXIS, -1.5 * Math.PI);
        return { pole: pole.applyMatrix4(back), centre: centre.applyMatrix4(back) };
    })();
    const sky = new THREE.Mesh(new THREE.SphereGeometry(1, 32, 16), new THREE.ShaderMaterial({
        uniforms: { uZenith: skyU.uZenith, uHorizon: skyU.uHorizon, uNight: skyU.uNight, uSun: { value: SKY_SUN }, uSunCol: { value: SUN_COL },
                    uLut: { value: skyLut }, uNightZ: { value: NIGHT_ZENITH.clone().convertSRGBToLinear() }, uNightH: { value: NIGHT_HORIZON.clone().convertSRGBToLinear() },
                    uStarRot: { value: starRot }, uTime: { value: 0 }, uGalPole: { value: GAL.pole }, uGalCentre: { value: GAL.centre }, ...rayU },
        vertexShader: `varying vec3 vDir;
void main() {
	vDir = position;
	gl_Position = projectionMatrix * vec4(mat3(viewMatrix) * position, 1.0);
	gl_Position.z = gl_Position.w;
}`,
        fragmentShader: `varying vec3 vDir;
uniform vec3 uZenith, uHorizon, uSun, uSunCol, uNightZ, uNightH;
uniform float uNight, uTime;
uniform sampler2D uLut;
uniform mat3 uStarRot;
uniform vec3 uGalPole, uGalCentre;
${RAYS > 0 ? RAY_GLSL : ""}

/* ---- the night sky, procedural but by the real sky's numbers ---- */
uvec3 pcg3(uvec3 v) {
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
	return v;
}
vec3 rnd3(ivec3 p) { return vec3(pcg3(uvec3(p))) * (1.0 / 4294967295.0); }
float vhash(vec3 p) { p = fract(p * 0.3183099 + 0.1) * 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
float vnoise(vec3 p) {
	vec3 i = floor(p), f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(vhash(i), vhash(i + vec3(1, 0, 0)), f.x), mix(vhash(i + vec3(0, 1, 0)), vhash(i + vec3(1, 1, 0)), f.x), f.y),
	           mix(mix(vhash(i + vec3(0, 0, 1)), vhash(i + vec3(1, 0, 1)), f.x), mix(vhash(i + vec3(0, 1, 1)), vhash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float fbm(vec3 p) {
	float v = 0.0, a = 0.5;
	for (int i = 0; i < 5; i++) { v += a * vnoise(p); p = p * 2.03 + 3.1; a *= 0.5; }
	return v;
}
/* cube-face cells, tan-warped so each covers about the same patch of sky */
const float STAR_N = 120.0;
vec3 faceDir(int f, vec2 w) {
	vec2 uv = tan(w * 0.7853982);
	return f == 0 ? vec3(1.0, uv) : f == 1 ? vec3(-1.0, uv) : f == 2 ? vec3(uv.x, 1.0, uv.y) :
	       f == 3 ? vec3(uv.x, -1.0, uv.y) : f == 4 ? vec3(uv, 1.0) : vec3(uv, -1.0);
}
/* One star per cell, at a random place in it. Brightness: a sky has about
 * three times as many stars per magnitude fainter, so flux ~ r^-0.8 of a
 * uniform r - most are barely there, a few hundred stand out. Colour from the
 * real spread of star temperatures (most yellow-white), seen only in the
 * bright ones (the eye sees faint light without colour). Points smaller
 * than a pixel at any zoom; only the very brightest bloom, slightly.
 * gain: what dims or crowds them here (the Milky Way's dust, its density). */
vec3 starLayer(vec3 c, float px, float N, int seed, float f0, float cap, float gain) {
	vec3 a = abs(c);
	int f; vec2 uv;
	if (a.x >= a.y && a.x >= a.z) { f = c.x > 0.0 ? 0 : 1; uv = c.yz / a.x; }
	else if (a.y >= a.z) { f = c.y > 0.0 ? 2 : 3; uv = c.xz / a.y; }
	else { f = c.z > 0.0 ? 4 : 5; uv = c.xy / a.z; }
	vec2 g = (atan(uv) * 1.2732395 * 0.5 + 0.5) * N, cell = floor(g);
	float lowk = 1.0 - smoothstep(0.0, 0.6, c.y);
	vec3 sum = vec3(0.0);
	for (int j = -1; j <= 1; j++)
		for (int i = -1; i <= 1; i++) {
			vec2 k = cell + vec2(i, j);
			if (k.x < 0.0 || k.y < 0.0 || k.x >= N || k.y >= N) continue;
			vec3 r = rnd3(ivec3(int(k.x), int(k.y), f * 977 + seed));
			vec3 r2 = rnd3(ivec3(int(k.y) + 5003, int(k.x), f + seed));
			vec3 sd = normalize(faceDir(f, (k + 0.05 + 0.9 * r.xy) / N * 2.0 - 1.0));
			vec3 dv = c - sd;
			float t2 = dot(dv, dv);
			float flux = min(f0 * (pow(max(r.z, 1e-6), -0.8) - 1.0), cap) * gain;
			/* twinkling: the air's slow shimmer, more low down where the light crosses more of it */
			flux *= 1.0 + (0.05 + 0.25 * lowk) * sin(uTime * (1.1 + 1.4 * r2.y) + r2.z * 60.0) * sin(uTime * (0.6 + 0.8 * r2.z) + r2.y * 31.0);
			float sg = px * 0.5;
			float core = exp(-t2 / (2.0 * sg * sg));
			float glow = max(flux - 0.8, 0.0) * 0.025 * exp(-t2 / (4.5 * px * px));
			float T = r2.x;
			vec3 tint = T < 0.10 ? vec3(0.70, 0.80, 1.00) : T < 0.38 ? vec3(0.93, 0.96, 1.00) : T < 0.75 ? vec3(1.00, 0.95, 0.85) :
			            T < 0.95 ? vec3(1.00, 0.82, 0.62) : vec3(1.00, 0.68, 0.48);
			tint = mix(vec3(1.0), tint, smoothstep(0.04, 0.5, flux));
			sum += tint * (flux * core + glow);
		}
	return sum;
}
/* The Milky Way: the disc of our galaxy seen from inside, a band round a
 * great circle, wider and brighter towards its centre. What makes it read as
 * far away rather than as mist: it is mostly stars - a crowd of faint points
 * (a finer star layer, only in the band) over a dim glow of those too faint
 * to resolve - clumped into star clouds and cut by thin dark dust lanes that
 * dim the stars behind them too. In the sky's own frame, so it turns with
 * the stars. */
vec3 milkyWay(vec3 c, float px, out float dust) {
	vec3 gp = uGalPole, gc = uGalCentre;
	float b = dot(c, gp);
	float core = pow(0.5 + 0.5 * dot(c, gc), 5.0);
	float w = 0.12 + 0.14 * core + 0.04 * (fbm(c * 3.0) - 0.5);
	float band = exp(-b * b / (w * w));
	dust = 1.0;
	if (band < 0.02) return vec3(0.0);
	float clouds = fbm(c * 9.0);
	float dens = band * (0.25 + 0.75 * core) * (0.25 + 1.3 * clouds * clouds);
	/* lanes: thin filaments, the main rift along the middle */
	float rift = exp(-pow((b - 0.012 + 0.04 * (fbm(c * 5.0 + 9.0) - 0.5)) / (0.022 + 0.02 * core), 2.0));
	float fil = smoothstep(0.52, 0.62, fbm(c * 22.0 + 4.0)) * smoothstep(0.45, 0.6, fbm(c * 6.0 + 2.0));
	dust = (1.0 - 0.85 * rift * smoothstep(0.35, 0.6, fbm(c * 14.0 + 4.0))) * (1.0 - 0.7 * fil * band);
	vec3 glowCol = mix(vec3(0.72, 0.80, 1.0), vec3(1.0, 0.90, 0.78), core * core);
	vec3 glow = glowCol * dens * dust * 0.03 * (0.6 + 0.8 * fbm(c * 70.0));
	vec3 crowd = starLayer(c, px, 420.0, 7919, 0.0025, 0.12, dens * dust * 1.6);
	return glow + crowd;
}

void main() {
	vec3 d = normalize(vDir);
	/* the LUT by the angle from the sun's bearing and the elevation */
	float az = abs(atan(d.z, d.x) - atan(uSun.z, uSun.x));
	az = min(az, 6.2831853 - az) / 3.1415927;
	float el = sqrt(clamp(asin(clamp(d.y, -1.0, 1.0)) / 1.5707963, 0.0, 1.0));
	vec2 uv = (vec2(az, el) * vec2(${SKY_W - 1}.0, ${SKY_H - 1}.0) + 0.5) / vec2(${SKY_W}.0, ${SKY_H}.0);
	vec3 col = texture2D(uLut, uv).rgb + uNight * mix(uNightH, uNightZ, smoothstep(0.0, 0.55, d.y));
	/* looking down past the map's edge: a deeper haze, not one flat colour */
	col = mix(col, mix(uHorizon, uZenith, 0.45), smoothstep(0.0, -0.9, d.y));
	float s = max(dot(d, uSun), 0.0);
	col += uSunCol * pow(s, 120.0) * 0.5;
	col = mix(col, uSunCol * 6.0, smoothstep(0.9993, 0.9996, s));
	/* at night the moon, opposite the sun, and stars */
	float m = max(-dot(d, uSun), 0.0);
	col += uNight * (vec3(0.30, 0.34, 0.45) * pow(m, 60.0) * 0.06 + vec3(0.75, 0.80, 0.95) * smoothstep(0.99955, 0.9998, m));
	if (${STARS > 0} && uNight > 0.0 && d.y > -0.02) {
		vec3 c = uStarRot * d;
		float px = length(fwidth(d));
		float dust;
		vec3 night = milkyWay(c, px, dust);
		night += starLayer(c, px, STAR_N, 13, 0.0012, 1.6, dust);
		/* low down the light crosses more air: dimmer and redder (k per airmass, b > g > r) */
		float X = 1.0 / (max(d.y, 0.0) + 0.025);
		night *= exp(-vec3(0.10, 0.15, 0.27) * (X - 1.0)) * smoothstep(-0.02, 0.01, d.y);
		/* the moon's glow outshines what is near it */
		night *= 1.0 - 0.8 * pow(m, 30.0);
		col += uNight * ${STARS.toFixed(2)} * night;
	}
	${RAYS > 0 ? "if (uRayHere > 0.5) col += quadMean(lightShafts(cameraPosition, cameraPosition + d * " + RAY_FAR + ".0));" : ""}
	gl_FragColor = vec4(col, 1.0);
	#include <tonemapping_fragment>
	#include <colorspace_fragment>
}`,
        side: THREE.BackSide, depthWrite: false, fog: false,
    }));
    sky.frustumCulled = false;
    sky.renderOrder = 1e9;
    scene.add(sky);

    Object.assign(app, { STAR_AXIS, atmosphere, sky, starRot, starRot4 });
}
