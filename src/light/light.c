/* Baked light per corner: the sun's visibility windows along its arc,
 * sky light from cosine-weighted rays, block light from rays to the
 * emitters, all cached per corner (corner_light). */
#include "prism.h"

/* The light follows the 2.5D renderer (render/src/light/light.c): a low
 * warm sun from the -x side with soft shadows, plus cool skylight
 * from the dome, multiplied onto sRGB texels with no tone mapping (the
 * viewer undoes the GPU's linear maths). Faces are shaded per axis the
 * way its walls are: south 0.86, east 0.71. */
int occ_top; /* highest grid y holding an occluder */

static float sky_dirs[SKY_RAYS][3]; /* cosine-weighted around +y */

/* The sun moves as in the game, rising in the east (+x) and setting in the
 * west, its path tilted SUN_TILT towards -z so noon still casts a shadow;
 * at 136 degrees it stands where the 2.5D renderer's does. */
#define SUN_SAMPLES 16 /* coarse visibility samples along the arc, refined after */

#define SUN_REFINE 4   /* bisection steps per change found between two */

#define SUN_TILT 0.2475f

void sun_at(float th, float d[3]) {
    d[0] = cosf(th);
    d[1] = sinf(th) * cosf(SUN_TILT);
    d[2] = -sinf(th) * sinf(SUN_TILT);
}

void normalize3(float v[3]) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int i = 0; i < 3; i++) v[i] /= l;
}

void setup_dirs(void) {
    const float golden = 2.39996323f;
    for (int i = 0; i < SKY_RAYS; i++) {
        float u = (i + 0.5f) / SKY_RAYS;
        float r = sqrtf(u), phi = i * golden;
        sky_dirs[i][0] = r * cosf(phi);
        sky_dirs[i][1] = sqrtf(1 - u);
        sky_dirs[i][2] = r * sinf(phi);
    }
}

int16_t hm_top[HM_W * HM_W], hm_big[HM_BW * HM_BW];

/* the cell size the ray at (ix, iy, iz) can skip, 0 for none */
static inline int hm_skip(int ix, int iy, int iz) {
    if (iy > hm_big[(iz / HM_BIG) * HM_BW + ix / HM_BIG]) return HM_BIG;
    return iy > hm_top[(iz / HM_CELL) * HM_W + ix / HM_CELL] ? HM_CELL : 0;
}

/* where along o + t d the ray leaves the cell x cell column round (ix, iz) */
static inline float hm_exit(const float o[3], const float d[3], int ix, int iz, int cell) {
    float t = 1e30f;
    const int c0[2] = { ix & ~(cell - 1), iz & ~(cell - 1) };
    for (int k = 0; k < 2; k++) {
        int a = k * 2;
        float e = d[a] > 1e-7f ? (c0[k] + cell - o[a]) / d[a] : d[a] < -1e-7f ? (c0[k] - o[a]) / d[a] : 1e30f;
        t = e < t ? e : t;
    }
    return t;
}

/* voxels a ray visits at most: more than a ray of the longest reach
 * crosses, a guard should the skipping below ever stop moving it on */
#define RAY_STEPS 4096

/* the DDA's state at distance t along the ray */
static void dda_at(const float o[3], const float d[3], float t, const int step[3], int ip[3], float tmax[3]) {
    for (int a = 0; a < 3; a++) {
        ip[a] = (int)floorf(o[a] + d[a] * t);
        tmax[a] = step[a] > 0 ? (ip[a] + 1 - o[a]) / d[a] : step[a] < 0 ? (ip[a] - o[a]) / d[a] : 1e30f;
    }
}

/* Leaves let light through: a ray lighting a leaf face goes on through up
 * to LEAF_PASS leaf voxels (its own block among them), as light gets
 * through a canopy's outer layers; deeper in, the leaves stop it. Shader
 * packs do this with the shadow map's depth (subsurface scattering). */
#define LEAF_PASS 2

/* leaf: lighting a leaf face. Spends one of *pass on a leaf voxel; 1 if
 * the occluder at (x, y, z) stops the ray. */
static inline int ray_stops(int x, int y, int z, int *pass) {
    if (*pass > 0 && states[grid[gidx(x, y, z)]].leaves) { --*pass; return 0; }
    return 1;
}

/* 1 if the ray from o along d leaves the grid (or goes maxd) without
 * entering an occluding voxel (past leaf's LEAF_PASS leaves). The starting
 * voxel is not tested. */
int ray_free(const float o[3], const float d[3], float maxd, int leaf) {
    int pass = leaf ? LEAF_PASS : 0;
    int ix = (int)floorf(o[0]), iy = (int)floorf(o[1]), iz = (int)floorf(o[2]);
    int step[3]; float tmax[3], tdel[3];
    const int ip[3] = { ix, iy, iz };
    for (int a = 0; a < 3; a++) {
        if (d[a] > 1e-7f) { step[a] = 1; tdel[a] = 1 / d[a]; tmax[a] = (ip[a] + 1 - o[a]) / d[a]; }
        else if (d[a] < -1e-7f) { step[a] = -1; tdel[a] = -1 / d[a]; tmax[a] = (o[a] - ip[a]) / -d[a]; }
        else { step[a] = 0; tdel[a] = 1e30f; tmax[a] = 1e30f; }
    }
    float tin = 0; /* where the ray entered the current voxel */
    for (int n = 0; n < RAY_STEPS; n++) {
        int a = tmax[0] < tmax[1] ? (tmax[0] < tmax[2] ? 0 : 2) : (tmax[1] < tmax[2] ? 1 : 2);
        if (tmax[a] > maxd) return 1;
        tin = tmax[a];
        if (a == 0) ix += step[0]; else if (a == 1) iy += step[1]; else iz += step[2];
        tmax[a] += tdel[a];
    test:
        if (ix < 0 || iz < 0 || ix >= GW || iz >= GW || iy >= GH) return 1;
        if (iy > occ_top && step[1] >= 0) return 1;
        if (iy < 0) return 0;
        if (occ_at(ix, iy, iz) && !occ_miss(ix, iy, iz, o, d, tin, fminf(fminf(tmax[0], tmax[1]), tmax[2]))
            && ray_stops(ix, iy, iz, &pass)) return 0;
        int cell = step[1] >= 0 ? hm_skip(ix, iy, iz) : 0;
        float te = cell ? hm_exit(o, d, ix, iz, cell) : 0;
        /* only ever forwards: a ray starting on a cell's edge has its exit
           behind it, and is left to step over the edge first */
        if (te > tin + 1e-3f) {
            if (te > maxd) return 1;
            int v[3];
            float tm[3];
            dda_at(o, d, te + 1e-4f, step, v, tm);
            /* a ray nearly along the edge may not get past it: step on */
            if (v[0] / cell != ix / cell || v[2] / cell != iz / cell) {
                ix = v[0]; iy = v[1]; iz = v[2];
                memcpy(tmax, tm, sizeof tm);
                tin = te;
                n++;
                goto test;
            }
        }
    }
    return 1;
}

/* Sun shadows are split by how far along the ray the topmost blocker
 * ends: near ones (a step, a fence, a flower) are left to the viewer's
 * shadow map, which draws them hard; far ones (trees, buildings, terrain)
 * stay baked and soft. Fades over NEAR0..NEAR1 blocks. The viewer's map
 * holds the faces nearest the sun, i.e. the same distance. Thin blockers
 * (far_occl: a fence, a lamp post, a fence used as a branch) count here
 * as whole voxels: drawn by the map however far, their long shadows broke
 * up at a low sun, and a crown built of them cast none of its own. They
 * are left to the map further, to THIN_NEAR1: a post is far thinner than
 * the voxel the bake takes it as, lit only at block corners, and a lamp
 * post's shadow came out as a crisp foot and then a gap before the blur. */
#define NEAR0 4.0f

#define NEAR1 6.0f

#define THIN_NEAR0 8.0f

#define THIN_NEAR1 12.0f

/* Plants (up_lit: a flower, a vine hung from a fence branch) are cut-out
 * planes: each one the ray crosses past PLANT_NEAR (nearer, the viewer's
 * plant map has it) lets PLANT_PASS through, and the windows, all or
 * nothing, count the sun as blocked once under half is left, so a sparse
 * plant casts no baked shadow and a canopy of them a soft one. */
#define PLANT_NEAR 2.0f

#define PLANT_PASS 0.75f

/* Share of the sun ray left to the bake: 1 when nothing blocks it or the
 * topmost blocker ends nearer than NEAR0. For a leaf face (leaf) the bake
 * keeps every blocker, near ones too, past its LEAF_PASS leaves: the
 * viewer's map would cut a canopy's shade up by the leaves' holes. */
static float ray_far_vis(const float o[3], const float d[3], float maxd, int leaf) {
    int pass = leaf ? LEAF_PASS : 0;
    int ix = (int)floorf(o[0]), iy = (int)floorf(o[1]), iz = (int)floorf(o[2]);
    int step[3]; float tmax[3], tdel[3];
    const int ip[3] = { ix, iy, iz };
    for (int a = 0; a < 3; a++) {
        if (d[a] > 1e-7f) { step[a] = 1; tdel[a] = 1 / d[a]; tmax[a] = (ip[a] + 1 - o[a]) / d[a]; }
        else if (d[a] < -1e-7f) { step[a] = -1; tdel[a] = -1 / d[a]; tmax[a] = (o[a] - ip[a]) / -d[a]; }
        else { step[a] = 0; tdel[a] = 1e30f; tmax[a] = 1e30f; }
    }
    float end = -1; /* where the latest run of blockers ended */
    int inside = 0, thin = 0, endthin = 0; /* the run all thin blockers */
    float pl = 1; /* the sun left past the plants so far */
    for (int n = 0; n < RAY_STEPS; n++) {
        int a = tmax[0] < tmax[1] ? (tmax[0] < tmax[2] ? 0 : 2) : (tmax[1] < tmax[2] ? 1 : 2);
        float t = tmax[a];
        if (t > maxd) break;
        if (a == 0) ix += step[0]; else if (a == 1) iy += step[1]; else iz += step[2];
        tmax[a] += tdel[a];
    test:
        if (ix < 0 || iz < 0 || ix >= GW || iz >= GW || iy >= GH) break;
        if (iy > occ_top && step[1] >= 0) break;
        if (iy < 0) return 0;
        int solid = occ_at(ix, iy, iz) && !occ_miss(ix, iy, iz, o, d, t, fminf(fminf(tmax[0], tmax[1]), tmax[2]));
        int o2 = solid || focc_at(ix, iy, iz);
        if (!o2 && !leaf && t >= PLANT_NEAR && pocc_at(ix, iy, iz) && (pl *= PLANT_PASS) < 0.5f) return 0;
        if (o2 && leaf && (o2 = ray_stops(ix, iy, iz, &pass))) return 0;
        if (o2 && t >= (solid ? NEAR1 : THIN_NEAR1)) return 0;
        if (!o2 && inside) { end = t; endthin = thin; }
        if (o2) thin = (inside ? thin : 1) && !solid;
        inside = o2;
        int cell = !o2 && step[1] >= 0 ? hm_skip(ix, iy, iz) : 0;
        float te = cell ? hm_exit(o, d, ix, iz, cell) : 0;
        if (te > t + 1e-3f) {
            t = te;
            if (t > maxd) break;
            int v[3];
            float tm[3];
            dda_at(o, d, t + 1e-4f, step, v, tm);
            /* a ray nearly along the edge may not get past it: step on */
            if (v[0] / cell != ix / cell || v[2] / cell != iz / cell) {
                ix = v[0]; iy = v[1]; iz = v[2];
                memcpy(tmax, tm, sizeof tm);
                n++;
                goto test;
            }
        }
    }
    if (inside) return 0;
    if (end < 0) return 1;
    float w = endthin ? (end - THIN_NEAR0) / (THIN_NEAR1 - THIN_NEAR0) : (end - NEAR0) / (NEAR1 - NEAR0);
    w = w < 0 ? 0 : w > 1 ? 1 : w;
    return 1 - w * w * (3 - 2 * w);
}

/* The game's block light at a face at p facing n: the voxels just in
 * front of it, interpolated (its smooth lighting averages the four around
 * each corner), skipping solid ones that give no light, 0..1. */
static float block_flood_at(const float p[3], const float n[3]) {
    float s[3];
    int i0[3];
    float f[3];
    for (int a = 0; a < 3; a++) {
        s[a] = p[a] + n[a] * 0.5f - 0.5f;
        i0[a] = (int)floorf(s[a]);
        f[a] = s[a] - i0[a];
    }
    float sum = 0, wsum = 0;
    for (int k = 0; k < 8; k++) {
        int x = i0[0] + (k & 1), y = i0[1] + (k >> 1 & 1), z = i0[2] + (k >> 2 & 1);
        float w = ((k & 1) ? f[0] : 1 - f[0]) * ((k >> 1 & 1) ? f[1] : 1 - f[1]) * ((k >> 2 & 1) ? f[2] : 1 - f[2]);
        if (w <= 0 || x < 0 || z < 0 || x >= GW || z >= GW || y < 0 || y >= GH) continue;
        size_t i = gidx(x, y, z);
        const state_t *st = &states[grid[i]];
        if (st->cull && !st->emit) continue;
        sum += w * blkl[i];
        wsum += w;
    }
    return wsum <= 0 ? 0 : sum / wsum / 15;
}

/* 1 if the segment from o along unit d reaches voxel t (within maxd)
 * without entering an occluding voxel before it. */
static int ray_to(const float o[3], const float d[3], float maxd, int tx, int ty, int tz) {
    int ix = (int)floorf(o[0]), iy = (int)floorf(o[1]), iz = (int)floorf(o[2]);
    int step[3]; float tmax[3], tdel[3];
    const int ip[3] = { ix, iy, iz };
    for (int a = 0; a < 3; a++) {
        if (d[a] > 1e-7f) { step[a] = 1; tdel[a] = 1 / d[a]; tmax[a] = (ip[a] + 1 - o[a]) / d[a]; }
        else if (d[a] < -1e-7f) { step[a] = -1; tdel[a] = -1 / d[a]; tmax[a] = (o[a] - ip[a]) / -d[a]; }
        else { step[a] = 0; tdel[a] = 1e30f; tmax[a] = 1e30f; }
    }
    for (;;) {
        if (ix == tx && iy == ty && iz == tz) return 1;
        int a = tmax[0] < tmax[1] ? (tmax[0] < tmax[2] ? 0 : 2) : (tmax[1] < tmax[2] ? 1 : 2);
        if (tmax[a] > maxd) return 1;
        float tin = tmax[a];
        if (a == 0) ix += step[0]; else if (a == 1) iy += step[1]; else iz += step[2];
        tmax[a] += tdel[a];
        if (ix == tx && iy == ty && iz == tz) return 1;
        if (ix < 0 || iz < 0 || ix >= GW || iz >= GW || iy < 0 || iy >= GH) return 1;
        if (occ_at(ix, iy, iz) && !occ_miss(ix, iy, iz, o, d, tin, fminf(fminf(tmax[0], tmax[1]), tmax[2]))) return 0;
    }
}

/* Block light at a face at p facing n, 0..1: each light in reach that the
 * face turns towards and nothing blocks, by the game's level for the
 * straight-line distance and the face's angle to it, so lights cast
 * shadows and model what they light; plus BLOCK_FILL of the light
 * bounced into shadow: the game's own, which creeps round corners, or
 * where a light is in sight, its straight-line level, which is more. The
 * game's falls off by steps along the axes, a diamond round each light,
 * so neither it nor its reach may bound what is in sight: that is round.
 * Lights combine as 1 - (1 - a)(1 - b)... */
#define BLOCK_FILL 0.30f

/* where in a light's block the rays to it end: its middle and eight
 * points towards its corners */
#define BLK_TAPS 9

static const float BLK_TAP[BLK_TAPS][3] = {
    { 0.5f, 0.5f, 0.5f },
    { 0.1f, 0.1f, 0.1f }, { 0.9f, 0.1f, 0.1f }, { 0.1f, 0.9f, 0.1f }, { 0.9f, 0.9f, 0.1f },
    { 0.1f, 0.1f, 0.9f }, { 0.9f, 0.1f, 0.9f }, { 0.1f, 0.9f, 0.9f }, { 0.9f, 0.9f, 0.9f },
};

static float lvl_curve(float l) { return l / (4 - 3 * l); }

static float block_light_at(const float p[3], const float n[3]) {
    if (!blkl) return 0;
    float flood = block_flood_at(p, n), dark = 1;
    float o[3] = { p[0] + n[0] * 0.1f, p[1] + n[1] * 0.1f, p[2] + n[2] * 0.1f };
    int c0[3], c1[3];
    for (int a = 0; a < 3; a++) {
        int lim = (a == 1 ? EMIT_CH : EMIT_CW) - 1;
        c0[a] = (int)floorf((o[a] - 15) / EMIT_CELL);
        c1[a] = (int)floorf((o[a] + 15) / EMIT_CELL);
        c0[a] = c0[a] < 0 ? 0 : c0[a];
        c1[a] = c1[a] > lim ? lim : c1[a];
    }
    for (int cy = c0[1]; cy <= c1[1]; cy++)
        for (int cz = c0[2]; cz <= c1[2]; cz++)
            for (int cx = c0[0]; cx <= c1[0]; cx++) {
                size_t c = ((size_t)cy * EMIT_CW + cz) * EMIT_CW + cx;
                for (uint32_t k = emc[c]; k < emc[c + 1]; k++) {
                    const emitter_t *e = &emv[k];
                    float d[3] = { e->x + 0.5f - o[0], e->y + 0.5f - o[1], e->z + 0.5f - o[2] };
                    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    if (dist >= e->e || dist < 1e-4f) continue;
                    for (int a = 0; a < 3; a++) d[a] /= dist;
                    float ndl = n[0] * d[0] + n[1] * d[1] + n[2] * d[2];
                    if (ndl <= 0) continue;
                    float l = lvl_curve((e->e - dist) / 15) * (0.3f + 0.7f * ndl);
                    if (l * dark < 0.004f) continue;
                    /* in sight of how much of the light's block: a lantern is no
                     * point, and as one, an edge a block or two from it cast a
                     * hard shadow blown up across the ground, squaring its pool */
                    int seen = 0;
                    for (int k = 0; k < BLK_TAPS; k++) {
                        float t[3] = { e->x + BLK_TAP[k][0] - o[0], e->y + BLK_TAP[k][1] - o[1], e->z + BLK_TAP[k][2] - o[2] };
                        float tl = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
                        for (int a = 0; a < 3; a++) t[a] /= tl;
                        seen += ray_to(o, t, tl + 1, e->x, e->y, e->z);
                    }
                    if (!seen) continue;
                    l *= (float)seen / BLK_TAPS;
                    dark *= 1 - l;
                    if ((e->e - dist) / 15 > flood) flood = (e->e - dist) / 15;
                }
            }
    return 1 - dark * (1 - BLOCK_FILL * lvl_curve(flood));
}

#define SKY_RANGE 20.0f

#define WATER_EXT 0.10f /* light lost per block of water above */

#define WATER_MIN_DEPTH 0.5f /* tint even the shallowest underwater faces */

/* The light's falloff for a face's normal, the 2.5D renderer's per axis.
 * Leaves get half of it: a canopy reads as one soft mass, not as cubes
 * with a dark side each. */
float face_shade(const float n[3], int leaf) {
    float s = n[1] * n[1] * (n[1] > 0 ? 1.0f : 0.5f) + n[2] * n[2] * 0.86f + n[0] * n[0] * 0.71f;
    return leaf ? 0.5f + 0.5f * s : s;
}

float sun_only = -1;

void light_one_sun(float th) { sun_only = th; }

/* a leaf face takes the sun from behind too, through its leaves */
static int sun_sees(const float sp[3], const float n[3], float th, int leaf) {
    float d[3];
    sun_at(th, d);
    return (leaf || n[0] * d[0] + n[1] * d[1] + n[2] * d[2] > 0) && ray_far_vis(sp, d, 200, leaf) >= 0.5f;
}

/* Where along the arc (0..pi) the sun gets past far blockers to p, facing
 * n, as up to two windows w[0]..w[1], w[2]..w[3] in 255ths of the arc; an
 * empty one has start > end. Sampled SUN_SAMPLES times, each change between
 * two samples found by bisection. Past two windows the smallest window or
 * gap goes (a gap closed, a window dropped). Returns the window count. */
static int sun_window(const float p[3], const float n[3], int leaf, uint8_t w[4]) {
    w[0] = 0; w[1] = 255; w[2] = 255; w[3] = 0;
    /* start just clear of a lower block's 1/16 lip (dirt path, farmland),
     * whose neighbours would otherwise shade a whole corner */
    float sp[3] = { p[0] + n[0] * 0.1f, p[1] + n[1] * 0.1f, p[2] + n[2] * 0.1f };
    if (sun_only >= 0) { /* one angle: the whole arc or none of it */
        if (sun_sees(sp, n, sun_only, leaf)) return 1;
        w[0] = 255; w[1] = 0;
        return 0;
    }
    const float PI = 3.14159265f, step = PI / SUN_SAMPLES;
    float iv[SUN_SAMPLES + 1][2];
    int nw = 0, prev = 0;
    float prev_th = 0;
    for (int i = 0; i < SUN_SAMPLES; i++) {
        float th = (i + 0.5f) * step;
        int v = sun_sees(sp, n, th, leaf);
        if (v != prev) {
            float lo = i ? prev_th : 0, hi = th;
            if (i)
                for (int k = 0; k < SUN_REFINE; k++) {
                    float mid = (lo + hi) * 0.5f;
                    if (sun_sees(sp, n, mid, leaf) == v) hi = mid; else lo = mid;
                }
            float edge = i ? (lo + hi) * 0.5f : 0;
            if (v) iv[nw][0] = edge;
            else iv[nw++][1] = edge;
        }
        prev = v;
        prev_th = th;
    }
    if (prev) iv[nw++][1] = PI;
    while (nw > 2) {
        int best = 0, gap = 0;
        float bl = 1e9f;
        for (int i = 0; i < nw; i++) {
            float l = iv[i][1] - iv[i][0];
            if (l < bl) { bl = l; best = i; gap = 0; }
            if (i + 1 < nw && iv[i + 1][0] - iv[i][1] < bl) { bl = iv[i + 1][0] - iv[i][1]; best = i; gap = 1; }
        }
        if (gap) iv[best][1] = iv[best + 1][1];
        for (int i = best + gap; i + 1 < nw; i++) memcpy(iv[i], iv[i + 1], sizeof iv[i]);
        nw--;
    }
    /* in steps of 4 (2.8 degrees), which packs much smaller */
    for (int i = 0; i < nw; i++) {
        int a = (int)lrintf(iv[i][0] / PI * 63.75f) * 4, b = (int)lrintf(iv[i][1] / PI * 63.75f) * 4;
        w[i * 2] = (uint8_t)(a > 255 ? 255 : a);
        w[i * 2 + 1] = (uint8_t)(b > 255 ? 255 : b);
    }
    if (nw == 0) { w[0] = 255; w[1] = 0; }
    return nw;
}

/* Light at p for normal n, with a full sun (out: a face square on to it,
 * nothing in the way) and with no sun at all (dark). The viewer adds
 * (out - dark) scaled by where the sun is and sun_mask. blk3 is the
 * block light alone at full strength; dark holds BLOCK_DAY of it. */
static void bake(const float p[3], const float n[3], int leaf, float out[3], float dark[3], float blk3[3], float prm[6]) {
    float o[3] = { p[0] + n[0] * 0.02f, p[1] + n[1] * 0.02f, p[2] + n[2] * 0.02f };
    for (int c = 0; c < 3; c++) blk3[c] = 0;
    prm[0] = -1; prm[1] = 0; prm[2] = 1; prm[3] = prm[4] = prm[5] = 0;
    const float sun = 1;
    /* sky: the whole upper dome seen from the air in front of the face, so
     * an open wall sees as much sky as open ground */
    float so[3] = { p[0] + n[0] * 0.3f, p[1] + n[1] * 0.3f, p[2] + n[2] * 0.3f };
    int open = 0;
    for (int i = 0; i < SKY_RAYS; i++) open += ray_free(so, sky_dirs[i], SKY_RANGE, 0);
    float sky = SKY_FLOOR + (1 - SKY_FLOOR) * (float)open / SKY_RAYS;
    float shade = face_shade(n, leaf);
    float blk = block_light_at(p, n);
    prm[0] = (float)open; prm[1] = blk;
    float lit[3][3];
    for (int c = 0; c < 3; c++) {
        for (int k = 0; k < 2; k++) {
            float l = SKY_COL[c] * sky + BLOCK_DAY * BLOCK_COL[c] * blk + (k ? 0 : SUN_COL[c] * sun);
            l = l < LIGHT_MIN ? LIGHT_MIN : l > LIGHT_MAX ? LIGHT_MAX : l;
            lit[k][c] = l * shade * ALBEDO;
        }
        /* at full strength, no floor: unlit corners stay 0 */
        float l = BLOCK_COL[c] * blk;
        lit[2][c] = (l > BLOCK_MAX ? BLOCK_MAX : l) * shade * ALBEDO;
    }
    /* orthonormal basis (t, b, n) for the underwater samples */
    const float a[3] = { fabsf(n[0]) < 0.9f ? 1.0f : 0.0f, fabsf(n[0]) < 0.9f ? 0.0f : 1.0f, 0 };
    float t[3] = { a[1] * n[2] - a[2] * n[1], a[2] * n[0] - a[0] * n[2], a[0] * n[1] - a[1] * n[0] }, b[3];
    normalize3(t);
    b[0] = n[1] * t[2] - n[2] * t[1];
    b[1] = n[2] * t[0] - n[0] * t[2];
    b[2] = n[0] * t[1] - n[1] * t[0];
    /* Under water: dim with depth and take on the water's colour. A vertex
     * on a cell boundary (a shore) touches dry and wet cells, so average the
     * four cells around it in the face's plane. */
    float wc[3], asum = 0;
    int wet = 0;
    for (int c = 0; c < 3; c++) out[c] = dark[c] = 0;
    for (int k = 0; k < 4; k++) {
        float su = (k & 1) ? 0.45f : -0.45f, sv = (k & 2) ? 0.45f : -0.45f, sp[3];
        for (int a = 0; a < 3; a++) sp[a] = o[a] + t[a] * su + b[a] * sv;
        int ox = (int)floorf(sp[0]), oy = (int)floorf(sp[1]), oz = (int)floorf(sp[2]);
        float att = 1;
        if (states[gget(ox, oy, oz)].fluid == FLUID_WATER) {
            int y = oy;
            while (y + 1 < GH && states[gget(ox, y + 1, oz)].fluid == FLUID_WATER) y++;
            float depth = (float)y + states[gget(ox, y, oz)].fluid_h / 16.0f - o[1];
            att = expf(-WATER_EXT * ((depth < 0 ? 0 : depth) + WATER_MIN_DEPTH));
            asum += att;
            if (!wet++) {
                biome_color_at(BC_WATER, o, wc);
                for (int c = 0; c < 3; c++) wc[c] = lin_to_srgb(wc[c]);
            }
        }
        for (int c = 0; c < 3; c++)
            for (int j = 0; j < 3; j++) {
                float l = lit[j][c];
                (j == 2 ? blk3 : j ? dark : out)[c] += 0.25f * (att == 1 ? l : l * att * (0.75f + 0.25f * wc[c]) + (1 - att) * wc[c] * 0.22f);
            }
    }
    /* what the viewer works the water out from: how many samples are wet,
     * their light through it, a quarter of the sum, and its colour */
    if (wet) {
        prm[0] += 32 * wet;
        prm[2] = asum / 4;
        memcpy(prm + 3, wc, sizeof wc);
    }
}

#define LC_PROBE 32

#define BQ_ONE 8192.0f

lcache_t *lcache;
int lc_bits = LC_BITS;

static int lc_key(const float p[3], const float n[3], int leaf, uint32_t k[4]) {
    int axis = -1;
    for (int a = 0; a < 3; a++)
        if (n[a] == 1 || n[a] == -1) axis = a * 2 + (n[a] < 0);
    if (axis < 0) return 0;
    memcpy(k, p, 12);
    k[3] = (uint32_t)(axis | leaf << 3);
    return 1;
}

static lcache_t *lc_find(const uint32_t k[4], int claim) {
    uint32_t h = k[0] * 0x9e3779b1u ^ k[1] * 0x85ebca77u ^ k[2] * 0xc2b2ae3du ^ k[3] * 0x27d4eb2fu;
    h ^= h >> 15;
    for (int i = 0; i < LC_PROBE; i++) {
        lcache_t *e = &lcache[(h + (uint32_t)i) & ((1u << lc_bits) - 1)];
        uint32_t st = atomic_load_explicit(&e->state, memory_order_acquire);
        if (st == 2) { if (!memcmp(e->key, k, 16)) return e; continue; }
        if (st == 1) return NULL;
        if (!claim) return NULL;
        uint32_t want = 0;
        if (atomic_compare_exchange_strong_explicit(&e->state, &want, 1, memory_order_acquire, memory_order_relaxed)) return e;
        if (want == 2 && !memcmp(e->key, k, 16)) return e;
        if (want == 1) return NULL;
    }
    return NULL;
}

/* bake(), sun_window() and bounce() for p lit as facing n (a leaf face if
 * leaf), through the cache; the bounce rounded as the cache keeps it, so a
 * corner comes out the same either way */
int corner_light(const float p[3], const float n[3], int leaf, float L[5][3], uint8_t swin[4], float B[BOUNCE_ANGLES][3]) {
    uint32_t k[4];
    lcache_t *e = lcache && lc_key(p, n, leaf, k) ? lc_find(k, 1) : NULL;
    if (e && atomic_load_explicit(&e->state, memory_order_acquire) == 2) {
        memcpy(L, e->L, sizeof e->L);
        memcpy(swin, e->swin, 4);
        for (int k = 0; k < BOUNCE_ANGLES; k++)
            for (int c = 0; c < 3; c++) B[k][c] = e->bq[k][c] / BQ_ONE;
        return e->nwin;
    }
    bake(p, n, leaf, L[0], L[1], L[2], L[3]);
    int nwin = sun_window(p, n, leaf, swin);
    uint16_t bq[BOUNCE_ANGLES][3];
    bounce(p, n, B);
    for (int k = 0; k < BOUNCE_ANGLES; k++)
        for (int c = 0; c < 3; c++) {
            float v = B[k][c] * BQ_ONE;
            bq[k][c] = (uint16_t)(v > 65535 ? 65535 : lrintf(v));
            B[k][c] = bq[k][c] / BQ_ONE;
        }
    if (e) {
        memcpy(e->key, k, 16);
        memcpy(e->L, L, sizeof e->L);
        memcpy(e->swin, swin, 4);
        memcpy(e->bq, bq, sizeof bq);
        e->nwin = (uint8_t)nwin;
        atomic_store_explicit(&e->state, 2, memory_order_release);
    }
    return nwin;
}
