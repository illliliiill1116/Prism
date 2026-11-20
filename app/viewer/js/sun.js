/*
 * The sun and moon: where along their arc they are, and the light they give.
 *
 * The sun moves along the game's east-west arc: ?sun= its angle in
 * degrees (0 sunrise in the east, 180 sunset; 136 the 2.5D renderer's),
 * [ and ] step it, T sets it going at ?sunspeed= degrees a second (1).
 * Between whole degrees its shadows are blended, not redrawn.
 */
import * as THREE from "three";

export function setupSun(app) {
    const { q } = app;
    /* At night, added to what the atmosphere still scatters: the sky and what
     * it reflects, and the multiplier on the
     * baked skylight (in COLOR_0's terms, before the 2.2), a dim blue. */
    const NIGHT_ZENITH = new THREE.Color(0x05081a), NIGHT_HORIZON = new THREE.Color(0x1b2140);
    const NIGHT_REFL_HIGH = new THREE.Color(0x141a38), NIGHT_REFL_LOW = new THREE.Color(0x262c4c);
    const NIGHT_SKY = new THREE.Vector3(0.26, 0.29, 0.46);
    /* Moonlight: the moon, opposite the sun, walks the same arc by night, so
     * the baked sun windows hold it too; a cool dim gain on the warm _SUN. */
    const MOON_GAIN = new THREE.Vector3(0.40, 0.48, 0.80), MOON_GLINT = 0.35;
    const BLOCK_DAY = 0.5; /* as prism's */
    const NIGHT_BLOCK = 0.85; /* block light at night, of _BLK */
    const skyU = {
        uZenith: { value: new THREE.Color() }, uHorizon: { value: new THREE.Color() },
        uReflHigh: { value: new THREE.Color() }, uReflLow: { value: new THREE.Color() }, uNight: { value: 0 },
    };
    /* the sun's arc, as prism's sun_at */
    const SUN_TILT = 0.2475;
    const SUN_REF = 0.666; /* flat ground's diffuse at 136 degrees, lit as 1 */
    const SUN_DIR = new THREE.Vector3(); /* the light: the sun by day, the moon by night */
    const SKY_SUN = new THREE.Vector3(); /* the sun itself, for the sky */
    app.lightDeg = 0;
    app.sunDeg = q.has("sun") ? Number(q.get("sun")) : 136.4;
    const sunU = { uSunT: { value: 0 }, uSunGain: { value: new THREE.Vector3(1, 1, 1) }, uSkyCol: { value: new THREE.Vector3(1, 1, 1) }, uBlkGain: { value: BLOCK_DAY }, uGlint: { value: 1 } };

    /* Sky: a dome by view direction - the atmosphere's LUT (see atmosphere)
     * and the sun with a halo. Drawn last of the opaque things at the far plane, so
     * it only runs where nothing else is. */
    const SUN_COL = new THREE.Color();
    /* Places the sun at deg along its arc: its direction (shared by the sky,
     * water and the shadow map) and, low down, a dimmer and redder light. */
    function sunAt(deg, out) {
        const th = deg * Math.PI / 180;
        return out.set(Math.cos(th), Math.sin(th) * Math.cos(SUN_TILT), -Math.sin(th) * Math.sin(SUN_TILT));
    }
    function placeSun(deg) {
        app.sunDeg = ((deg % 360) + 360) % 360;
        sunAt(app.sunDeg, SKY_SUN);
        app.lightDeg = app.sunDeg <= 180 ? app.sunDeg : app.sunDeg - 180;
        sunAt(app.lightDeg, SUN_DIR);
        sunU.uSunT.value = app.lightDeg / 180 * 255;
        const up = SKY_SUN.y, w = THREE.MathUtils.smoothstep(up, 0.05, 0.45);
        if (app.sunDeg <= 180) {
            const i = THREE.MathUtils.smoothstep(up, 0.0, 0.12);
            sunU.uSunGain.value.set(i, i * (0.55 + 0.45 * w), i * (0.3 + 0.7 * w));
            sunU.uGlint.value = i;
        } else {
            /* zero at both horizons, so the hand-over at sunset is seamless */
            const i = THREE.MathUtils.smoothstep(SUN_DIR.y, 0.0, 0.15);
            sunU.uSunGain.value.copy(MOON_GAIN).multiplyScalar(i);
            sunU.uGlint.value = i * MOON_GLINT;
        }
        /* dusk: the sky dims from a little before sunset to well after */
        const n = 1 - THREE.MathUtils.smoothstep(up, -0.22, 0.08);
        sunU.uSkyCol.value.set(1, 1, 1).lerp(NIGHT_SKY, n);
        sunU.uBlkGain.value = THREE.MathUtils.lerp(BLOCK_DAY, NIGHT_BLOCK, n);
        skyU.uNight.value = n;
        app.starRot.setFromMatrix4(app.starRot4.makeRotationAxis(app.STAR_AXIS, -app.sunDeg * Math.PI / 180));
        app.atmosphere(n);
        /* the shafts' light: the sun as it comes through the air, by night the moon's, much fainter */
        if (app.sunDeg <= 180) app.rayU.uRayCol.value.copy(SUN_COL);
        else app.rayU.uRayCol.value.setRGB(MOON_GAIN.x, MOON_GAIN.y, MOON_GAIN.z).multiplyScalar(0.12 * sunU.uGlint.value / MOON_GLINT);
    }

    Object.assign(app, { BLOCK_DAY, NIGHT_HORIZON, NIGHT_REFL_HIGH, NIGHT_REFL_LOW, NIGHT_ZENITH, SKY_SUN, SUN_COL, SUN_DIR,
                       SUN_REF, SUN_TILT, placeSun, skyU, sunAt, sunU });
}
