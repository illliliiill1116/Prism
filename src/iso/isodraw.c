/*
 * isodraw: level 0 drawn on the CPU into an image, in the 2.5D renderer's
 * projection (sx = (x - z) S, sy = (x + z) S / 2 + (320 - y) S + S / 2,
 * S pixels a block), lit as the viewer lights it (viewer/js/materials.js,
 * sun.js) with the sun at one angle: the baked sky, block and bounced light
 * and sun windows, the near shadows from a shadow map along the sun, the
 * shade lift and the glow of light-giving blocks. Left out: the effects
 * (bloom, grade, haze), ripples, glints and plants' wind.
 *
 * A world area is drawn window by window (iso_draw), each into the part
 * of the image it covers, in bands of rows, a thread each. Per band: the
 * opaque and cut-out triangles' nearest one per pixel (a visibility
 * buffer, so each pixel is shaded once), the nearest water surface, then
 * the shading; glass behind the water, the water seen through to what is
 * under it, glass in front.
 *
 * A window's core is the part of it that is its own to draw; the rest is
 * there for what the core's pixels see through water and for its shadows.
 * A pixel goes to the image only where the nearest thing in it is the
 * core's. Rays into the view go towards -x and -z, so a window drawn after
 * those at lower x and z (iso_draw's caller goes by x + z) is in front of
 * them where they meet, and draws over them.
 */
#include "prism.h"

#define SUB 16  /* subpixel steps per pixel */
#define BAND 32 /* output rows per band */
#define SBAND 64 /* shadow map rows per job */

/* as the viewer's */
#define BLOCK_DAY_V 0.5f
#define NIGHT_BLOCK 0.85f
#define SUN_TILT 0.2475f
#define SUN_REF 0.666f
#define SUN_EASE 2.5f
#define AMB_LIFT 0.6f
#define GLOW 5.0f
#define LEAF_SIDE 0.35f
#define LEAF_BACK 0.6f
#define LEAF_TINT 0.6f
#define PLANT_SSS 0.35f
#define BOUNCE_GAIN 6.0f
#define BOUNCE_CAP 0.15f
#define NEAR0 4.0f
#define NEAR1 6.0f
#define THIN_NEAR0 8.0f
#define THIN_NEAR1 12.0f
#define PLANT_CAST 0.5f
#define PLANT_BIAS 0.15f
/* the light on cut-away walls (linear), the right (+x) and left (+z) sides */
#define CUT_X 0.30f
#define CUT_Z 0.40f
static const float NIGHT_SKY[3] = { 0.26f, 0.29f, 0.46f }, MOON_GAIN[3] = { 0.40f, 0.48f, 0.80f };
static const float WATER_ABS[3] = { 0.11f, 0.05f, 0.04f };
/* the sky the water mirrors, linear: the viewer's atmosphere at noon and at night */
static const float REFL_HIGH[2][3] = { { 0.20f, 0.36f, 0.66f }, { 0.0070f, 0.0103f, 0.0395f } };
static const float REFL_LOW[2][3] = { { 0.50f, 0.60f, 0.72f }, { 0.0194f, 0.0251f, 0.0722f } };

enum { Q_SHADOWED = 1, Q_PLANT = 2, Q_LEAF = 4, Q_CORE = 8 };

/* A quad as drawn: screen corners in 1/SUB pixels from the image's
 * corner, depth (x + y + z: larger is nearer) and atlas texels, and its
 * light (li, in iso_t's ql; NO_LIGHT for a quad that never shows, which
 * goes unlit). Corners 0, 3, 1 and 1, 3, 2 make its triangles, as
 * emit_quad's. */
typedef struct {
    int32_t X[4], Y[4];
    float d[4], u[4], v[4];
    uint32_t li;
    uint8_t mat, flags;
} iq_t;

#define NO_LIGHT UINT32_MAX

/* A quad's light: without the sun (dark) and with all of it (lit) in
 * linear terms, where it is in the shadow map, its glow, and for water
 * what it mirrors. */
typedef struct {
    float dark[4][3], lit[4][3];
    float s[4][3]; /* shadow map texel x, y; blocks along the sun */
    float slope[2]; /* the face's own plane in the map: blocks per texel */
    float glow, F, sky[3];
} iql_t;

typedef struct {
    float dir[3], T, gain[3], sky[3], blk, night;
} sun_t;

/* The near shadow map: per texel the surface nearest the sun (blocks
 * along it), whether that is thin (a fence: its shadow reaches further),
 * and the nearest plant apart. */
typedef struct {
    int w, h;
    float tpb, ox, oy; /* texels per block; the map's corner in texels */
    float ax[3][3]; /* u, v, sun */
    float *solid, *plant;
    uint8_t *thin;
} smap_t;

typedef struct {
    const canvas_t *atlas;
    /* the atlas in linear terms at the level the pixels sample: texels a
     * pixel spans, each level half the last (colour weighted by alpha) */
    float *mip;
    int mw, mh, lvl;
    float lin[256];
    uint8_t enc[4096];
    sun_t sun;
    iso_image_t *img;
    int px0, py0;  /* the window's box in the image, output pixels */
    int scale, ss, W, H; /* the window's box, internal (supersampled) pixels */
    int core[4];   /* world x0, z0, x1, z1, inclusive */
    iq_t *q;
    iql_t *ql;
    size_t nq;
    uint32_t *bin, *bin0; /* triangles per band, CSR */
    int nbands;
    smap_t sm;
    int shadows;
    float Vn[3];
    _Atomic uint8_t *seen; /* light_defer: per quad, it shows in the core's pixels */
} iso_t;

static float smoothstepf(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }

static void sun_vec(float deg, float o[3]) {
    float th = deg * 3.14159265f / 180;
    o[0] = cosf(th); o[1] = sinf(th) * cosf(SUN_TILT); o[2] = -sinf(th) * sinf(SUN_TILT);
}

/* sun.js's placeSun */
static void place_sun(sun_t *s, float deg) {
    deg = fmodf(fmodf(deg, 360) + 360, 360);
    float sky_sun[3];
    sun_vec(deg, sky_sun);
    float light = deg <= 180 ? deg : deg - 180;
    sun_vec(light, s->dir);
    s->T = light / 180 * 255;
    float up = sky_sun[1], w = smoothstepf(0.05f, 0.45f, up);
    if (deg <= 180) {
        float i = smoothstepf(0, 0.12f, up);
        s->gain[0] = i; s->gain[1] = i * (0.55f + 0.45f * w); s->gain[2] = i * (0.3f + 0.7f * w);
    } else {
        float i = smoothstepf(0, 0.15f, s->dir[1]);
        for (int c = 0; c < 3; c++) s->gain[c] = MOON_GAIN[c] * i;
    }
    s->night = 1 - smoothstepf(-0.22f, 0.08f, up);
    for (int c = 0; c < 3; c++) s->sky[c] = 1 + (NIGHT_SKY[c] - 1) * s->night;
    s->blk = BLOCK_DAY_V + (NIGHT_BLOCK - BLOCK_DAY_V) * s->night;
}

static float sun_in(float a, float b, float T) {
    return a > b ? 0 : smoothstepf(a - SUN_EASE, a + SUN_EASE, T) * (1 - smoothstepf(b - SUN_EASE, b + SUN_EASE, T));
}

/* materials.js's sunWindows */
static float sun_windows(const uint8_t w8[4], float T) {
    float w[4] = { w8[0], w8[1], w8[2], w8[3] };
    if (w[0] <= 0) w[0] = -9;
    if (w[2] <= 0) w[2] = -9;
    if (w[1] >= 252) w[1] = 264;
    if (w[3] >= 252) w[3] = 264;
    float a = sun_in(w[0], w[1], T), b = sun_in(w[2], w[3], T);
    return a > b ? a : b;
}

static float powlin(float x) { return x <= 0 ? 0 : powf(x, 2.2f); }

/* ---- the shadow map ---- */

static void smap_project(const smap_t *M, const float p[3], float o[3]) {
    for (int k = 0; k < 2; k++)
        o[k] = (p[0] * M->ax[k][0] + p[1] * M->ax[k][1] + p[2] * M->ax[k][2]) * M->tpb - (k ? M->oy : M->ox);
    o[2] = p[0] * M->ax[2][0] + p[1] * M->ax[2][1] + p[2] * M->ax[2][2];
}

typedef struct {
    const iso_t *I;
    mbuf_t *mb;
    smap_t *M;
    float *sv; /* per vertex of OPAQUE then MASK: map x, y, t */
    size_t nv0;
    uint32_t *bin, *bin0;
    int nb;
    _Atomic int next;
} sjob_t;

static int tex_alpha(const iso_t *I, float u, float v) {
    int x = (int)u, y = (int)v;
    x = x < 0 ? 0 : x >= (int)I->atlas->width ? (int)I->atlas->width - 1 : x;
    y = y < 0 ? 0 : y >= (int)I->atlas->height ? (int)I->atlas->height - 1 : y;
    return canvas_row(I->atlas, (uint32_t)y)[x * 4 + 3];
}

/* One triangle into the map's rows r0..r1: the nearest to the sun per texel centre. */
static void smap_tri(sjob_t *J, const float *a, const float *b, const float *c, const float *ua, const float *ub,
                     const float *uc, int kind, int thin, int r0, int r1) {
    smap_t *M = J->M;
    float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (fabsf(area) < 1e-12f) return;
    float inv = 1 / area;
    float mnx = fminf(a[0], fminf(b[0], c[0])), mxx = fmaxf(a[0], fmaxf(b[0], c[0]));
    float mny = fminf(a[1], fminf(b[1], c[1])), mxy = fmaxf(a[1], fmaxf(b[1], c[1]));
    int x0 = (int)ceilf(mnx - 0.5f), x1 = (int)floorf(mxx - 0.5f), y0 = (int)ceilf(mny - 0.5f), y1 = (int)floorf(mxy - 0.5f);
    if (x0 < 0) x0 = 0;
    if (x1 >= M->w) x1 = M->w - 1;
    if (y0 < r0) y0 = r0;
    if (y1 >= r1) y1 = r1 - 1;
    for (int y = y0; y <= y1; y++) {
        float py = y + 0.5f;
        for (int x = x0; x <= x1; x++) {
            float px = x + 0.5f;
            float w0 = ((b[0] - px) * (c[1] - py) - (b[1] - py) * (c[0] - px)) * inv;
            float w1 = ((c[0] - px) * (a[1] - py) - (c[1] - py) * (a[0] - px)) * inv;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float t = w0 * a[2] + w1 * b[2] + w2 * c[2];
            size_t i = (size_t)y * M->w + x;
            float *dst = kind == 2 ? &M->plant[i] : &M->solid[i];
            if (t <= *dst) continue;
            if (kind && tex_alpha(J->I, w0 * ua[0] + w1 * ub[0] + w2 * uc[0], w0 * ua[1] + w1 * ub[1] + w2 * uc[1]) < 128) continue;
            *dst = t;
            if (kind != 2) M->thin[i] = (uint8_t)thin;
        }
    }
}

static void *smap_worker(void *arg) {
    sjob_t *J = arg;
    const iso_t *I = J->I;
    for (;;) {
        int b = atomic_fetch_add(&J->next, 1);
        if (b >= J->nb) break;
        int r0 = b * SBAND, r1 = r0 + SBAND;
        for (uint32_t k = J->bin0[b]; k < J->bin0[b + 1]; k++) {
            uint32_t t = J->bin[k];
            size_t qi = t >> 1;
            int m = qi < J->nv0 / 4 ? MAT_OPAQUE : MAT_MASK;
            size_t lq = m == MAT_OPAQUE ? qi : qi - J->nv0 / 4;
            const mbuf_t *mb = &J->mb[m];
            const uint8_t *C = &mb->col[lq * 4 * VSTRIDE];
            /* what it casts into: plants their own map, leaves as whole blocks */
            int kind = 0;
            if (m == MAT_MASK) kind = C[7] == 0 ? 2 : C[7] == LEAF_ALPHA ? 0 : 1;
            int thin = C[7] == THIN_ALPHA;
            static const int TV[2][3] = { { 0, 3, 1 }, { 1, 3, 2 } };
            const int *tv = TV[t & 1];
            const float *sv = &J->sv[qi * 12];
            float uv[3][2];
            for (int i = 0; i < 3; i++) {
                uv[i][0] = mb->uv[(lq * 4 + tv[i]) * 2] * I->atlas->width;
                uv[i][1] = mb->uv[(lq * 4 + tv[i]) * 2 + 1] * I->atlas->height;
            }
            smap_tri(J, sv + tv[0] * 3, sv + tv[1] * 3, sv + tv[2] * 3, uv[0], uv[1], uv[2], kind, thin, r0, r1);
        }
    }
    return NULL;
}

/* Bins triangles (two per quad) by the rows of size rows they touch:
 * ylo/yhi per quad, in rows. */
static void bin_rows(size_t nq, const float *ylo, const float *yhi, int rows, int nb, uint32_t **bin, uint32_t **bin0) {
    uint32_t *cnt = calloc((size_t)nb + 1, sizeof *cnt);
    for (size_t q = 0; q < nq; q++) {
        if (ylo[q] > yhi[q]) continue;
        int b0 = (int)floorf(ylo[q] / rows), b1 = (int)floorf(yhi[q] / rows);
        if (b0 < 0) b0 = 0;
        if (b1 >= nb) b1 = nb - 1;
        for (int b = b0; b <= b1; b++) cnt[b + 1] += 2;
    }
    for (int b = 0; b < nb; b++) cnt[b + 1] += cnt[b];
    uint32_t *out = xmalloc((size_t)cnt[nb] * sizeof *out), *fill = xmalloc((size_t)nb * sizeof *fill);
    memcpy(fill, cnt, (size_t)nb * sizeof *fill);
    for (size_t q = 0; q < nq; q++) {
        if (ylo[q] > yhi[q]) continue;
        int b0 = (int)floorf(ylo[q] / rows), b1 = (int)floorf(yhi[q] / rows);
        if (b0 < 0) b0 = 0;
        if (b1 >= nb) b1 = nb - 1;
        for (int b = b0; b <= b1; b++) {
            out[fill[b]++] = (uint32_t)(q * 2);
            out[fill[b]++] = (uint32_t)(q * 2 + 1);
        }
    }
    free(fill);
    *bin = out;
    *bin0 = cnt;
}

static void build_smap(iso_t *I, mbuf_t mb[MAT_COUNT], int nthreads) {
    smap_t *M = &I->sm;
    const float *w = I->sun.dir;
    float ref[3] = { 0, 0, 1 };
    if (fabsf(w[2]) > 0.9f) { ref[0] = 1; ref[2] = 0; }
    float u[3] = { ref[1] * w[2] - ref[2] * w[1], ref[2] * w[0] - ref[0] * w[2], ref[0] * w[1] - ref[1] * w[0] };
    normalize3(u);
    float v[3] = { w[1] * u[2] - w[2] * u[1], w[2] * u[0] - w[0] * u[2], w[0] * u[1] - w[1] * u[0] };
    memcpy(M->ax[0], u, sizeof u); memcpy(M->ax[1], v, sizeof v); memcpy(M->ax[2], w, sizeof v);
    M->tpb = clampf(I->scale * I->ss / 2.0f, 2, 8);
    M->ox = M->oy = 0;
    size_t nv0 = mb[MAT_OPAQUE].nv, nv = nv0 + mb[MAT_MASK].nv, nq = nv / 4;
    float *sv = xmalloc(nv * 3 * sizeof *sv);
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
    for (size_t i = 0; i < nv; i++) {
        const float *p = i < nv0 ? &mb[MAT_OPAQUE].pos[i * 3] : &mb[MAT_MASK].pos[(i - nv0) * 3];
        smap_project(M, p, &sv[i * 3]);
        for (int k = 0; k < 2; k++) {
            if (sv[i * 3 + k] < lo[k]) lo[k] = sv[i * 3 + k];
            if (sv[i * 3 + k] > hi[k]) hi[k] = sv[i * 3 + k];
        }
    }
    if (!nv) lo[0] = lo[1] = hi[0] = hi[1] = 0;
    M->ox = floorf(lo[0]) - 2; M->oy = floorf(lo[1]) - 2;
    M->w = (int)(hi[0] - M->ox) + 3; M->h = (int)(hi[1] - M->oy) + 3;
    for (size_t i = 0; i < nv; i++) { sv[i * 3] -= M->ox; sv[i * 3 + 1] -= M->oy; }
    size_t n = (size_t)M->w * M->h;
    M->solid = xmalloc(n * sizeof *M->solid);
    M->plant = xmalloc(n * sizeof *M->plant);
    M->thin = calloc(n, 1);
    for (size_t i = 0; i < n; i++) M->solid[i] = M->plant[i] = -1e30f;
    float *ylo = xmalloc(nq * sizeof *ylo), *yhi = xmalloc(nq * sizeof *yhi);
    for (size_t q = 0; q < nq; q++) {
        ylo[q] = 1e30f; yhi[q] = -1e30f;
        for (int i = 0; i < 4; i++) {
            float y = sv[(q * 4 + i) * 3 + 1];
            if (y < ylo[q]) ylo[q] = y;
            if (y > yhi[q]) yhi[q] = y;
        }
    }
    sjob_t J = { .I = I, .mb = mb, .M = M, .sv = sv, .nv0 = nv0, .nb = (M->h + SBAND - 1) / SBAND };
    bin_rows(nq, ylo, yhi, SBAND, J.nb, &J.bin, &J.bin0);
    free(ylo); free(yhi);
    atomic_store(&J.next, 0);
    pthread_t th[256];
    for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, smap_worker, &J);
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    free(J.bin); free(J.bin0); free(sv);
}

/* 0..1: how much a blocker less than NEAR1 blocks towards the sun hides
 * s (map x, y, t), as nearShadowIn's four-tap branch: each tap blends
 * the four texels around it, each texel tested against the face's own
 * plane there. */
static float near_shadow(const smap_t *M, const float s[3], const float slope[2], int plant, float r) {
    float bias = plant ? PLANT_BIAS : 0.01f + 0.5f / M->tpb;
    const float o[4][2] = { { r, 0.5f * r }, { -0.5f * r, r }, { -r, -0.5f * r }, { 0.5f * r, -r } };
    float k = 0, solid = 1;
    for (int j = 0; j < 4; j++) {
        float tx = s[0] + o[j][0] - 0.5f, ty = s[1] + o[j][1] - 0.5f;
        int ix = (int)floorf(tx), iy = (int)floorf(ty);
        float fx = tx - ix, fy = ty - iy, h[4];
        for (int c = 0; c < 4; c++) {
            int x = ix + (c & 1), y = iy + (c >> 1);
            h[c] = 0;
            if (x < 0 || y < 0 || x >= M->w || y >= M->h) { solid = 0; continue; }
            size_t i = (size_t)y * M->w + x;
            float z = s[2] + slope[0] * (x + 0.5f - s[0]) + slope[1] * (y + 0.5f - s[1]);
            float diff = M->solid[i] - z, n0 = M->thin[i] ? THIN_NEAR0 : NEAR0, n1 = M->thin[i] ? THIN_NEAR1 : NEAR1;
            if (!(diff >= bias && diff <= n1)) solid = 0;
            h[c] = diff >= bias ? 1 - smoothstepf(n0, n1, diff) : 0;
        }
        float kt = (h[0] + (h[1] - h[0]) * fx) * (1 - fy) + (h[2] + (h[3] - h[2]) * fx) * fy;
        if (!plant) {
            int x = (int)(s[0] + o[j][0]), y = (int)(s[1] + o[j][1]);
            if (x >= 0 && y >= 0 && x < M->w && y < M->h) {
                float z = s[2] + slope[0] * o[j][0] + slope[1] * o[j][1];
                float diff = M->plant[(size_t)y * M->w + x] - z;
                float p = diff >= bias ? PLANT_CAST * (1 - smoothstepf(NEAR0, NEAR1, diff)) : 0;
                if (p > kt) kt = p;
            }
        }
        k += kt * 0.25f;
    }
    return k > solid ? k : solid;
}

/* ---- preparing the quads ---- */

typedef struct {
    iso_t *I;
    mbuf_t *mb;
    size_t q0[MAT_COUNT + 1]; /* first quad of each material in I->q */
    double ox, oy; /* the image's corner in projected pixels */
    int64_t from, to;
    double cx, cz;
} pjob_t;

static void project(const iso_t *I, double cx, double cz, const float p[3], double *sx, double *sy, float *d) {
    double S = (double)I->scale * I->ss, wx = p[0] + cx, wy = p[1], wz = p[2] + cz;
    *sx = (wx - wz) * S;
    *sy = (wx + wz) * S / 2 + (320 - wy) * S + S / 2;
    *d = (float)(p[0] + p[1] + p[2]);
}

/* Lights quad lq of material m as the viewer's vertex and fragment
 * stages would, all but what varies per pixel (the near shadow). Returns
 * 0 for a face turned away. */
static int prep_quad(const iso_t *I, const mbuf_t *mb, int m, size_t lq, double cx, double cz, double ox, double oy, iq_t *o, iql_t *l) {
    const float *P = &mb->pos[lq * 12];
    float e1[3], e2[3], n[3];
    for (int a = 0; a < 3; a++) { e1[a] = P[3 + a] - P[a]; e2[a] = P[9 + a] - P[a]; }
    n[0] = e2[1] * e1[2] - e2[2] * e1[1];
    n[1] = e2[2] * e1[0] - e2[0] * e1[2];
    n[2] = e2[0] * e1[1] - e2[1] * e1[0];
    float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (nl < 1e-9f) return 0;
    for (int a = 0; a < 3; a++) n[a] /= nl;
    float facing = n[0] * I->Vn[0] + n[1] * I->Vn[1] + n[2] * I->Vn[2];
    /* glass both sides, as the viewer's; water one, or where the mesh is
     * cut (the region's edge) its far side shows as a wall of water */
    if (m != MAT_BLEND) { if (facing <= 0) return 0; }
    else if (facing < 0) for (int a = 0; a < 3; a++) n[a] = -n[a];
    const sun_t *S = &I->sun;
    const uint8_t *C0 = &mb->col[lq * 4 * VSTRIDE];
    /* A side against a column with nothing in it (the mesh's edge, a chunk
     * never generated) is a cut through the ground: lit flat, as the 2.5D
     * renderer's cut-away walls, so its layers show rather than the dark
     * the bake gives the inside of a hill. */
    if (m == MAT_OPAQUE && fabsf(n[1]) < 0.01f) {
        int gx = (int)floorf((P[0] + P[6]) * 0.5f + n[0] * 0.5f) + MARGIN + REG / 2;
        int gz = (int)floorf((P[2] + P[8]) * 0.5f + n[2] * 0.5f) + MARGIN + REG / 2;
        if (gx < 0 || gz < 0 || gx >= GW || gz >= GW || coltop[gz * GW + gx] < 0) {
            float L = fabsf(n[0]) > 0.5f ? CUT_X : CUT_Z;
            o->mat = (uint8_t)m;
            o->flags = 0;
            l->glow = 0;
            for (int i = 0; i < 4; i++) {
                double sx, sy;
                project(I, cx, cz, &P[i * 3], &sx, &sy, &o->d[i]);
                o->X[i] = (int32_t)llround((sx - ox) * SUB);
                o->Y[i] = (int32_t)llround((sy - oy) * SUB);
                o->u[i] = mb->uv[(lq * 4 + i) * 2] * I->atlas->width;
                o->v[i] = mb->uv[(lq * 4 + i) * 2 + 1] * I->atlas->height;
                for (int c = 0; c < 3; c++) l->dark[i][c] = l->lit[i][c] = L * (0.4f + 0.6f * S->sky[c]);
            }
            return 1;
        }
    }
    int plant = C0[7] == 0, leaf = C0[7] == LEAF_ALPHA;
    o->mat = (uint8_t)m;
    o->flags = (uint8_t)((plant ? Q_PLANT : 0) | (leaf ? Q_LEAF : 0));
    l->glow = C0[3] / 255.0f;
    float ndl = n[0] * S->dir[0] + n[1] * S->dir[1] + n[2] * S->dir[2];
    float diff = clampf((plant ? S->dir[1] : ndl) / SUN_REF, 0, 1.3f);
    /* where it is in the map, pushed 1.5 texels off towards the sun */
    float ns[3] = { n[0], n[1], n[2] };
    if (plant) memcpy(ns, S->dir, sizeof ns);
    else if (ndl < 0) for (int a = 0; a < 3; a++) ns[a] = -ns[a];
    float push = I->shadows ? 1.5f / I->sm.tpb : 0;
    float bounce_up = fmaxf(sinf(S->T * 3.14159265f / 255), 0) * cosf(SUN_TILT);
    int any_sun = 0;
    for (int i = 0; i < 4; i++) {
        const float *p = &P[i * 3];
        double sx, sy;
        project(I, cx, cz, p, &sx, &sy, &o->d[i]);
        o->X[i] = (int32_t)llround((sx - ox) * SUB);
        o->Y[i] = (int32_t)llround((sy - oy) * SUB);
        o->u[i] = mb->uv[(lq * 4 + i) * 2] * I->atlas->width;
        o->v[i] = mb->uv[(lq * 4 + i) * 2 + 1] * I->atlas->height;
        const uint8_t *C = &mb->col[(lq * 4 + i) * VSTRIDE];
        float vc[3], sa[3];
        for (int c = 0; c < 3; c++) {
            float col = C[c] / 255.0f, blk = C[12 + c] / 255.0f;
            vc[c] = fmaxf(col - BLOCK_DAY_V * blk, 0) * S->sky[c] + blk * S->blk;
        }
        if (m == MAT_WATER) {
            for (int c = 0; c < 3; c++) l->dark[i][c] = l->lit[i][c] = powlin(vc[c] * 2) * 0.6f;
            continue;
        }
        if (C[20]) { /* bounce */
            float a = C[20] / 31.0f, b = a * a * bounce_up * BOUNCE_MAX * BOUNCE_GAIN;
            b /= 1 + b / BOUNCE_CAP;
            for (int c = 0; c < 3; c++) vc[c] += C[16 + c] * (b / BNC_Q) * S->gain[c];
        }
        float win = sun_windows(C + 8, S->T);
        for (int c = 0; c < 3; c++) sa[c] = C[4 + c] / 255.0f * win * S->gain[c];
        float sl[3];
        if (plant || leaf) {
            float mx = fmaxf(sa[0], fmaxf(sa[1], sa[2])), lt[3];
            if (mx < 1e-3f) mx = 1e-3f;
            for (int c = 0; c < 3; c++) lt[c] = 1 + (sa[c] / mx - 1) * LEAF_TINT;
            if (plant)
                for (int c = 0; c < 3; c++) sl[c] = sa[c] * (diff + PLANT_SSS / SUN_REF * lt[c]);
            else {
                float sss = LEAF_SIDE + (LEAF_BACK - LEAF_SIDE) * fmaxf(-ndl, 0);
                for (int c = 0; c < 3; c++) sl[c] = sa[c] * (fmaxf(ndl, 0) + sss * lt[c]) / SUN_REF;
            }
        } else
            for (int c = 0; c < 3; c++) sl[c] = sa[c] * diff;
        int lit = sl[0] + sl[1] + sl[2] > 0;
        any_sun |= lit;
        for (int c = 0; c < 3; c++) {
            l->dark[i][c] = powlin(vc[c] * 2);
            l->lit[i][c] = lit ? powlin((vc[c] + sl[c]) * 2) : l->dark[i][c];
        }
        if (I->shadows) {
            float ps[3] = { p[0] + ns[0] * push, p[1] + ns[1] * push, p[2] + ns[2] * push };
            smap_project(&I->sm, ps, l->s[i]);
        }
    }
    if (any_sun && !leaf && I->shadows) o->flags |= Q_SHADOWED;
    l->slope[0] = l->slope[1] = 0;
    if ((o->flags & Q_SHADOWED) && !plant) {
        /* the face's plane: t over map x, y, from corners 0, 1, 3 */
        float a[3], b[3];
        for (int k = 0; k < 3; k++) { a[k] = l->s[1][k] - l->s[0][k]; b[k] = l->s[3][k] - l->s[0][k]; }
        float det = a[0] * b[1] - a[1] * b[0];
        if (fabsf(det) > 1e-6f) {
            l->slope[0] = (a[2] * b[1] - b[2] * a[1]) / det;
            l->slope[1] = (a[0] * b[2] - b[0] * a[2]) / det;
        }
    }
    if (m == MAT_WATER) {
        const float *V = I->Vn;
        float cosT = clampf(facing < 0 ? -facing : facing, 0, 1), k = 1 - cosT;
        l->F = 0.02f + 0.98f * k * k * k * k * k;
        float ry = -V[1] + 2 * cosT * n[1];
        float t = clampf(ry * 1.2f, 0, 1), nt = S->night;
        for (int c = 0; c < 3; c++) {
            float hi = REFL_HIGH[0][c] + (REFL_HIGH[1][c] - REFL_HIGH[0][c]) * nt;
            float lo = REFL_LOW[0][c] + (REFL_LOW[1][c] - REFL_LOW[0][c]) * nt;
            l->sky[c] = lo + (hi - lo) * t;
        }
    }
    return 1;
}

static void prep_one(const pjob_t *J, int64_t k) {
    int m = 0;
    while ((size_t)k >= J->q0[m + 1]) m++;
    iq_t *o = &J->I->q[k];
    size_t lq = (size_t)k - J->q0[m];
    iql_t scratch;
    iql_t *l = o->li == NO_LIGHT ? &scratch : &J->I->ql[o->li];
    if (!prep_quad(J->I, &J->mb[m], m, lq, J->cx, J->cz, J->ox, J->oy, o, l)) { o->mat = 255; return; }
    {
        /* the block it belongs to: just behind its middle */
        const float *P = &J->mb[m].pos[lq * 12];
        float e1[3], e2[3], n[3];
        for (int a = 0; a < 3; a++) { e1[a] = P[3 + a] - P[a]; e2[a] = P[9 + a] - P[a]; }
        n[0] = e2[1] * e1[2] - e2[2] * e1[1];
        n[2] = e2[0] * e1[1] - e2[1] * e1[0];
        float nl = sqrtf(n[0] * n[0] + n[2] * n[2] + powf(e2[2] * e1[0] - e2[0] * e1[2], 2));
        int bx = (int)floor((P[0] + P[6]) * 0.5 - n[0] / nl * 0.01 + J->cx);
        int bz = (int)floor((P[2] + P[8]) * 0.5 - n[2] / nl * 0.01 + J->cz);
        const int *c = J->I->core;
        if (bx >= c[0] && bx <= c[2] && bz >= c[1] && bz <= c[3]) o->flags |= Q_CORE;
    }
}

static void *prep_worker(void *arg) {
    pjob_t *J = arg;
    for (int64_t k = J->from; k < J->to; k++) {
        J->I->q[k].li = light_defer ? NO_LIGHT : (uint32_t)k;
        prep_one(J, k);
    }
    return NULL;
}

/* light_defer: the quads that show (and the glass, which is shaded
 * wherever it is in front) lit, and prepared again with their light */
typedef struct {
    pjob_t p;
    _Atomic int64_t next;
} ljob_t;

static void *light_worker(void *arg) {
    ljob_t *L = arg;
    const pjob_t *J = &L->p;
    const iso_t *I = J->I;
    for (;;) {
        int64_t k0 = atomic_fetch_add(&L->next, 1024), k1 = k0 + 1024;
        if (k0 >= J->to) break;
        if (k1 > J->to) k1 = J->to;
        for (int64_t k = k0; k < k1; k++) {
            if (I->q[k].li == NO_LIGHT) continue;
            int m = 0;
            while ((size_t)k >= J->q0[m + 1]) m++;
            light_quad(&J->mb[m], (size_t)k - J->q0[m]);
            prep_one(J, k);
        }
    }
    return NULL;
}

/* ---- drawing a band ---- */

/* P_SEEN_*: the glass passes for where the nearest glass is, drawing nothing */
enum { P_VIS, P_WATER, P_GLASS_BACK, P_GLASS_FRONT, P_SEEN_BACK, P_SEEN_FRONT };

typedef struct {
    int y0, h; /* internal rows */
    uint32_t *vis, *wid, *gid; /* gid: the nearest glass drawn */
    float *dep, *wdep, *gdep, *col;
    uint32_t *glass;
    size_t nglass, cglass;
} band_t;

static const int TRI[2][3] = { { 0, 3, 1 }, { 1, 3, 2 } };

static void tex_at(const iso_t *I, float u, float v, float out[4]) {
    int x = (int)u >> I->lvl, y = (int)v >> I->lvl;
    x = x < 0 ? 0 : x >= I->mw ? I->mw - 1 : x;
    y = y < 0 ? 0 : y >= I->mh ? I->mh - 1 : y;
    memcpy(out, &I->mip[((size_t)y * I->mw + x) * 4], 4 * sizeof *out);
}

static void build_mip(iso_t *I) {
    int w = (int)I->atlas->width, h = (int)I->atlas->height;
    float *m = xmalloc((size_t)w * h * 4 * sizeof *m);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *t = canvas_row(I->atlas, (uint32_t)y) + x * 4;
            float *o = &m[((size_t)y * w + x) * 4];
            for (int c = 0; c < 3; c++) o[c] = I->lin[t[c]];
            o[3] = t[3] / 255.0f;
        }
    /* texels a pixel spans: a block's 16 over about scale pixels */
    float span = 16.0f / ((float)I->scale * I->ss);
    I->lvl = 0;
    while (span >= 1.5f && w > 1 && h > 1) {
        int nw = w / 2, nh = h / 2;
        float *n = xmalloc((size_t)nw * nh * 4 * sizeof *n);
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                float s[4] = { 0 };
                for (int k = 0; k < 4; k++) {
                    const float *p = &m[((size_t)(2 * y + (k >> 1)) * w + 2 * x + (k & 1)) * 4];
                    for (int c = 0; c < 3; c++) s[c] += p[c] * p[3];
                    s[3] += p[3];
                }
                float *o = &n[((size_t)y * nw + x) * 4];
                for (int c = 0; c < 3; c++) o[c] = s[3] > 0 ? s[c] / s[3] : 0;
                o[3] = s[3] / 4;
            }
        free(m);
        m = n; w = nw; h = nh;
        I->lvl++;
        span /= 2;
    }
    I->mip = m; I->mw = w; I->mh = h;
}

/* The colour of quad q at barycentrics b over its triangle tv, as the
 * viewer's fragment stage: texture x light, the near shadow, the shade
 * lift, the glow. */
static void quad_tex(const iso_t *I, const iq_t *q, const int tv[3], const float b[3], float tex[4]) {
    float u = 0, v = 0;
    for (int k = 0; k < 3; k++) {
        int i = tv[k];
        u += b[k] * q->u[i]; v += b[k] * q->v[i];
    }
    tex_at(I, u, v, tex);
}

static void shade(const iso_t *I, const iq_t *q, const int tv[3], const float b[3], float out[4]) {
    const iql_t *l = &I->ql[q->li];
    float dark[3] = { 0 }, lit[3] = { 0 }, s[3] = { 0 };
    for (int k = 0; k < 3; k++) {
        int i = tv[k];
        for (int c = 0; c < 3; c++) {
            dark[c] += b[k] * l->dark[i][c];
            lit[c] += b[k] * l->lit[i][c];
            s[c] += b[k] * l->s[i][c];
        }
    }
    float tex[4];
    quad_tex(I, q, tv, b, tex);
    if (q->flags & Q_SHADOWED) {
        float r = fmaxf(0.35f * I->sm.tpb / ((float)I->scale * I->ss), 0.7f);
        float k = near_shadow(&I->sm, s, l->slope, q->flags & Q_PLANT, r);
        for (int c = 0; c < 3; c++) lit[c] += (dark[c] - lit[c]) * k;
    }
    float dy = 0.2126f * dark[0] + 0.7152f * dark[1] + 0.0722f * dark[2];
    float ly = 0.2126f * lit[0] + 0.7152f * lit[1] + 0.0722f * lit[2];
    float lift = AMB_LIFT * clampf(dy / fmaxf(ly, 1e-4f), 0, 1);
    float ty = 0.2126f * tex[0] + 0.7152f * tex[1] + 0.0722f * tex[2];
    /* the viewer's glow at GLOW_D blocks away: half */
    float glow = l->glow * 0.5f * GLOW * smoothstepf(0.2f, 0.7f, ty);
    for (int c = 0; c < 3; c++) out[c] = tex[c] * (lit[c] + lift * dark[c]) + tex[c] * glow;
    out[3] = tex[3];
}

static void bary_at(const iq_t *q, const int tv[3], int px, int py, float b[3]) {
    double x = px * SUB + SUB / 2, y = py * SUB + SUB / 2;
    double x0 = q->X[tv[0]], y0 = q->Y[tv[0]], x1 = q->X[tv[1]], y1 = q->Y[tv[1]], x2 = q->X[tv[2]], y2 = q->Y[tv[2]];
    double area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    b[0] = (float)(((x1 - x) * (y2 - y) - (y1 - y) * (x2 - x)) / area);
    b[1] = (float)(((x2 - x) * (y0 - y) - (y2 - y) * (x0 - x)) / area);
    b[2] = 1 - b[0] - b[1];
}

static void raster(const iso_t *I, band_t *B, uint32_t tri, int pass) {
    const iq_t *q = &I->q[tri >> 1];
    int tv[3] = { TRI[tri & 1][0], TRI[tri & 1][1], TRI[tri & 1][2] };
    int64_t X[3], Y[3];
    for (int k = 0; k < 3; k++) { X[k] = q->X[tv[k]]; Y[k] = q->Y[tv[k]]; }
    int64_t area = (X[1] - X[0]) * (Y[2] - Y[0]) - (Y[1] - Y[0]) * (X[2] - X[0]);
    if (area == 0) return;
    if (area < 0) {
        int t = tv[1]; tv[1] = tv[2]; tv[2] = t;
        int64_t a = X[1]; X[1] = X[2]; X[2] = a;
        a = Y[1]; Y[1] = Y[2]; Y[2] = a;
        area = -area;
    }
    int64_t mnx = X[0], mxx = X[0], mny = Y[0], mxy = Y[0];
    for (int k = 1; k < 3; k++) {
        if (X[k] < mnx) mnx = X[k];
        if (X[k] > mxx) mxx = X[k];
        if (Y[k] < mny) mny = Y[k];
        if (Y[k] > mxy) mxy = Y[k];
    }
    /* pixel centres px * SUB + SUB / 2 within the box */
    int px0 = (int)((mnx - SUB / 2 + SUB - 1 + (int64_t)SUB * 1024) / SUB) - 1024, px1 = (int)((mxx - SUB / 2 + (int64_t)SUB * 1024) / SUB) - 1024;
    int py0 = (int)((mny - SUB / 2 + SUB - 1 + (int64_t)SUB * 1024) / SUB) - 1024, py1 = (int)((mxy - SUB / 2 + (int64_t)SUB * 1024) / SUB) - 1024;
    if (px0 < 0) px0 = 0;
    if (px1 >= I->W) px1 = I->W - 1;
    if (py0 < B->y0) py0 = B->y0;
    if (py1 >= B->y0 + B->h) py1 = B->y0 + B->h - 1;
    if (px0 > px1 || py0 > py1) return;
    /* edge k is opposite corner k; owned edges take the pixels on them */
    int64_t A[3], Bc[3], Cc[3];
    int bias[3];
    for (int k = 0; k < 3; k++) {
        int a = (k + 1) % 3, b = (k + 2) % 3;
        int64_t dx = X[b] - X[a], dy = Y[b] - Y[a];
        /* E(p) = dx (py - ya) - dy (px - xa) */
        A[k] = -dy; Bc[k] = dx; Cc[k] = dy * X[a] - dx * Y[a];
        bias[k] = (dy < 0 || (dy == 0 && dx > 0)) ? 0 : -1;
    }
    double inv = 1.0 / (double)area;
    for (int py = py0; py <= py1; py++) {
        int64_t cy = (int64_t)py * SUB + SUB / 2, cx = (int64_t)px0 * SUB + SUB / 2;
        int64_t w[3];
        for (int k = 0; k < 3; k++) w[k] = A[k] * cx + Bc[k] * cy + Cc[k];
        size_t row = (size_t)(py - B->y0) * I->W;
        for (int px = px0; px <= px1; px++, w[0] += A[0] * SUB, w[1] += A[1] * SUB, w[2] += A[2] * SUB) {
            if (w[0] + bias[0] < 0 || w[1] + bias[1] < 0 || w[2] + bias[2] < 0) continue;
            float b[3] = { (float)(w[0] * inv), (float)(w[1] * inv), 0 };
            b[2] = 1 - b[0] - b[1];
            float d = b[0] * q->d[tv[0]] + b[1] * q->d[tv[1]] + b[2] * q->d[tv[2]];
            size_t i = row + px;
            if (d <= B->dep[i]) continue;
            switch (pass) {
            case P_VIS:
                if (q->mat == MAT_MASK) {
                    float u = b[0] * q->u[tv[0]] + b[1] * q->u[tv[1]] + b[2] * q->u[tv[2]];
                    float v = b[0] * q->v[tv[0]] + b[1] * q->v[tv[1]] + b[2] * q->v[tv[2]];
                    float t4[4];
                    tex_at(I, u, v, t4);
                    if (t4[3] < (I->lvl ? 0.35f : 0.5f)) continue;
                }
                B->dep[i] = d;
                B->vis[i] = tri + 1;
                break;
            case P_WATER:
                if (d > B->wdep[i]) { B->wdep[i] = d; B->wid[i] = tri + 1; }
                break;
            default: {
                if ((pass == P_GLASS_BACK || pass == P_SEEN_BACK) != (d < B->wdep[i])) continue;
                float c[4];
                if (pass == P_SEEN_BACK || pass == P_SEEN_FRONT) quad_tex(I, q, tv, b, c); /* its alpha, unlit yet */
                else shade(I, q, tv, b, c);
                if (pass == P_GLASS_BACK || pass == P_GLASS_FRONT) {
                    float *o = &B->col[i * 4];
                    for (int k = 0; k < 3; k++) o[k] = o[k] * (1 - c[3]) + c[k] * c[3];
                    o[3] = o[3] + (1 - o[3]) * c[3];
                }
                if (c[3] > 0 && d > B->gdep[i]) { B->gdep[i] = d; B->gid[i] = tri + 1; }
            }
            }
        }
    }
}

static const iso_t *sort_iso; /* qsort has no context; set once before the bands */
static int cmp_glass(const void *a, const void *b) {
    const iq_t *qa = &sort_iso->q[*(const uint32_t *)a >> 1], *qb = &sort_iso->q[*(const uint32_t *)b >> 1];
    float da = qa->d[0] + qa->d[2], db = qb->d[0] + qb->d[2];
    return da < db ? -1 : da > db;
}

/* The band's nearest opaque or cut-out triangle and water per pixel, and
 * its glass listed. */
static void band_vis(const iso_t *I, band_t *B, int bi) {
    size_t n = (size_t)I->W * B->h;
    for (size_t i = 0; i < n; i++) {
        B->dep[i] = B->wdep[i] = B->gdep[i] = -1e30f;
        B->vis[i] = B->wid[i] = B->gid[i] = 0;
    }
    memset(B->col, 0, n * 4 * sizeof *B->col);
    B->nglass = 0;
    for (uint32_t k = I->bin0[bi]; k < I->bin0[bi + 1]; k++) {
        uint32_t t = I->bin[k];
        int m = I->q[t >> 1].mat;
        if (m == MAT_OPAQUE || m == MAT_MASK) raster(I, B, t, P_VIS);
        else if (m == MAT_BLEND) {
            if (B->nglass == B->cglass) {
                B->cglass = B->cglass ? B->cglass * 2 : 1024;
                B->glass = realloc(B->glass, B->cglass * sizeof *B->glass);
                if (!B->glass) { fprintf(stderr, "oom\n"); exit(1); }
            }
            B->glass[B->nglass++] = t;
        }
    }
    for (uint32_t k = I->bin0[bi]; k < I->bin0[bi + 1]; k++)
        if (I->q[I->bin[k] >> 1].mat == MAT_WATER) raster(I, B, I->bin[k], P_WATER);
    /* glass back to front (by its quad's middle) */
    qsort(B->glass, B->nglass, sizeof *B->glass, cmp_glass);
}

/* the triangle nearest in pixel i, + 1 (0 none) */
static uint32_t band_front(const band_t *B, size_t i) {
    float d = B->dep[i];
    uint32_t t = B->vis[i];
    if (B->wdep[i] > d) { d = B->wdep[i]; t = B->wid[i]; }
    if (B->gdep[i] > d) t = B->gid[i];
    return t;
}

/* light_defer: marks the quads the band's core pixels show, as nearest
 * or under the water or glass that is */
static void see_band(const iso_t *I, band_t *B, int bi) {
    band_vis(I, B, bi);
    for (size_t k = 0; k < B->nglass; k++) raster(I, B, B->glass[k], P_SEEN_BACK);
    for (size_t k = 0; k < B->nglass; k++) raster(I, B, B->glass[k], P_SEEN_FRONT);
    size_t n = (size_t)I->W * B->h;
    for (size_t i = 0; i < n; i++) {
        uint32_t t = band_front(B, i);
        if (!t || !(I->q[(t - 1) >> 1].flags & Q_CORE)) continue;
        if (B->vis[i]) atomic_store_explicit(&I->seen[(B->vis[i] - 1) >> 1], 1, memory_order_relaxed);
        if (B->wid[i]) atomic_store_explicit(&I->seen[(B->wid[i] - 1) >> 1], 1, memory_order_relaxed);
    }
}

static void draw_band(const iso_t *I, band_t *B, int bi) {
    size_t n = (size_t)I->W * B->h;
    band_vis(I, B, bi);
    for (int py = 0; py < B->h; py++)
        for (int px = 0; px < I->W; px++) {
            size_t i = (size_t)py * I->W + px;
            if (!B->vis[i]) continue;
            uint32_t t = B->vis[i] - 1;
            const iq_t *q = &I->q[t >> 1];
            if (q->li == NO_LIGHT) continue; /* not the core's nearest: dropped below */
            float b[3], c[4];
            bary_at(q, TRI[t & 1], px, py + B->y0, b);
            shade(I, q, TRI[t & 1], b, c);
            memcpy(&B->col[i * 4], c, 3 * sizeof *c);
            B->col[i * 4 + 3] = 1;
        }
    for (size_t k = 0; k < B->nglass; k++) raster(I, B, B->glass[k], P_GLASS_BACK);
    /* the water: what is under it, dimmed by the water it is seen through, and the sky it mirrors */
    for (int py = 0; py < B->h; py++)
        for (int px = 0; px < I->W; px++) {
            size_t i = (size_t)py * I->W + px;
            if (!B->wid[i]) continue;
            uint32_t t = B->wid[i] - 1;
            const iq_t *q = &I->q[t >> 1];
            if (q->li == NO_LIGHT) continue;
            const iql_t *l = &I->ql[q->li];
            float b[3], base[3] = { 0 };
            bary_at(q, TRI[t & 1], px, py + B->y0, b);
            for (int k = 0; k < 3; k++)
                for (int c = 0; c < 3; c++) base[c] += b[k] * l->dark[TRI[t & 1][k]][c];
            float *o = &B->col[i * 4];
            float path = B->dep[i] > -1e29f ? (B->wdep[i] - B->dep[i]) * 0.57735f : 1e9f;
            for (int c = 0; c < 3; c++) {
                float tr = expf(-path * WATER_ABS[c]);
                float under = o[c] * tr + base[c] * (1 - tr) * 0.5f;
                o[c] = under + (l->sky[c] - under) * l->F;
            }
            o[3] = 1;
        }
    for (size_t k = 0; k < B->nglass; k++) raster(I, B, B->glass[k], P_GLASS_FRONT);
    /* the pixels whose nearest surface is not the core's are another window's */
    for (size_t i = 0; i < n; i++) {
        uint32_t t = band_front(B, i);
        if (t && !(I->q[(t - 1) >> 1].flags & Q_CORE)) B->col[i * 4 + 3] = 0;
    }
}

typedef struct {
    const iso_t *I;
    band_t *bands;
    int first, count, see; /* see: see_band only */
    _Atomic int next, worker;
} djob_t;

static void *draw_worker(void *arg) {
    djob_t *J = arg;
    const iso_t *I = J->I;
    int ss = I->ss, ow = I->W / ss;
    band_t *B = &J->bands[atomic_fetch_add(&J->worker, 1)]; /* this thread's buffers */
    for (;;) {
        int k = atomic_fetch_add(&J->next, 1);
        if (k >= J->count) break;
        int bi = J->first + k;
        B->y0 = bi * BAND * ss;
        B->h = BAND * ss;
        if (B->y0 + B->h > I->H) B->h = I->H - B->y0;
        if (J->see) { see_band(I, B, bi); continue; }
        draw_band(I, B, bi);
        /* down to the output's pixels, in linear terms, into sRGB and over
         * what windows behind drew */
        int rows = B->h / ss;
        iso_image_t *img = I->img;
        for (int r = 0; r < rows; r++) {
            int y = I->py0 + bi * BAND + r;
            uint8_t *dst = NULL;
            for (int x = 0; x < ow; x++) {
                float s[4] = { 0 };
                for (int sy = 0; sy < ss; sy++)
                    for (int sx = 0; sx < ss; sx++) {
                        const float *p = &B->col[((size_t)(r * ss + sy) * I->W + x * ss + sx) * 4];
                        for (int c = 0; c < 3; c++) s[c] += p[c] * p[3];
                        s[3] += p[3];
                    }
                if (s[3] <= 0) continue;
                if (!dst) {
                    if (!img->rows[y]) {
                        img->rows[y] = calloc((size_t)img->w, 4);
                        if (!img->rows[y]) { fprintf(stderr, "out of memory for the image\n"); exit(1); }
                    }
                    dst = img->rows[y];
                }
                int gx = I->px0 + x;
                uint8_t *d = dst + (size_t)gx * 4;
                float a = s[3] / (ss * ss), da = d[3] / 255.0f, oa = a + da * (1 - a);
                for (int c = 0; c < 3; c++) {
                    float v = I->enc[(int)(clampf(s[c] / s[3], 0, 1) * 4095 + 0.5f)];
                    d[c] = (uint8_t)lrintf((v * a + d[c] * da * (1 - a)) / oa);
                }
                d[3] = (uint8_t)lrintf(oa * 255);
                if (gx < img->x0[y]) img->x0[y] = gx;
                if (gx > img->x1[y]) img->x1[y] = gx;
            }
        }
    }
    return NULL;
}

void iso_project(int scale, double wx, double wy, double wz, double *sx, double *sy) {
    *sx = (wx - wz) * scale;
    *sy = (wx + wz) * scale / 2.0 + (320 - wy) * scale + scale / 2.0;
}

int iso_image_init(iso_image_t *img, int scale, int x0, int z0, int x1, int z1, int ymin) {
    memset(img, 0, sizeof *img);
    /* the box's corners, ymin to the world's top */
    double lo[2] = { 1e300, 1e300 }, hi[2] = { -1e300, -1e300 };
    for (int k = 0; k < 8; k++) {
        double sx, sy;
        iso_project(scale, k & 1 ? x1 + 1 : x0, k & 2 ? 320 : ymin, k & 4 ? z1 + 1 : z0, &sx, &sy);
        if (sx < lo[0]) lo[0] = sx;
        if (sx > hi[0]) hi[0] = sx;
        if (sy < lo[1]) lo[1] = sy;
        if (sy > hi[1]) hi[1] = sy;
    }
    img->ox = (long)floor(lo[0]);
    img->oy = (long)floor(lo[1]);
    double w = ceil(hi[0]) - img->ox, h = ceil(hi[1]) - img->oy;
    if (w > 1 << 20 || h > 1 << 20) return -1;
    img->w = (int)w;
    img->h = (int)h;
    img->rows = calloc((size_t)img->h, sizeof *img->rows);
    img->x0 = xmalloc((size_t)img->h * sizeof *img->x0);
    img->x1 = xmalloc((size_t)img->h * sizeof *img->x1);
    if (!img->rows) return -1;
    for (int y = 0; y < img->h; y++) { img->x0[y] = img->w; img->x1[y] = -1; }
    return 0;
}

void iso_image_free(iso_image_t *img) {
    for (int y = 0; y < img->h; y++) free(img->rows[y]);
    free(img->rows); free(img->x0); free(img->x1);
    memset(img, 0, sizeof *img);
}

/* Writes what was drawn, cropped to it; *box gets its corner in projected
 * pixels and its size. */
int iso_image_write(const iso_image_t *img, const char *path, int nthreads, long box[4]) {
    int y0 = 0, y1 = img->h - 1, x0 = img->w, x1 = -1;
    while (y0 <= y1 && img->x1[y0] < 0) y0++;
    while (y1 >= y0 && img->x1[y1] < 0) y1--;
    for (int y = y0; y <= y1; y++) {
        if (img->x1[y] < 0) continue;
        if (img->x0[y] < x0) x0 = img->x0[y];
        if (img->x1[y] > x1) x1 = img->x1[y];
    }
    if (y0 > y1) { fprintf(stderr, "iso: nothing was drawn\n"); return 1; }
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    box[0] = img->ox + x0; box[1] = img->oy + y0; box[2] = w; box[3] = h;
    png_stream_t *st = png_stream_begin(path, (uint32_t)w, (uint32_t)h, 6, nthreads);
    if (!st) { fprintf(stderr, "can't write %s\n", path); return 1; }
    enum { ROWS = 64 };
    canvas_t band = canvas_create((uint32_t)w, ROWS);
    for (int y = y0; y <= y1; y += ROWS) {
        int n = y1 - y + 1 < ROWS ? y1 - y + 1 : ROWS;
        for (int r = 0; r < n; r++) {
            uint8_t *d = canvas_row(&band, (uint32_t)r);
            if (img->rows[y + r]) memcpy(d, img->rows[y + r] + (size_t)x0 * 4, (size_t)w * 4);
            else memset(d, 0, (size_t)w * 4);
        }
        canvas_t part = canvas_subview(&band, 0, 0, (uint32_t)w, (uint32_t)n);
        png_stream_write_rows(st, &part);
    }
    canvas_free(&band);
    png_result_t r = png_stream_finish(st);
    if (r != PNG_OK) { fprintf(stderr, "can't write %s: %s\n", path, png_result_str(r)); return 1; }
    return 0;
}

/* Draws the window's level 0 (mb, lit by the window loaded) into img,
 * the pixels whose nearest surface is in core (world x0, z0, x1, z1). */
int iso_draw(iso_image_t *img, mbuf_t mb[MAT_COUNT], const canvas_t *atlas, const iso_opts_t *o, const int core[4]) {
    static iso_t I;
    memset(&I, 0, sizeof I);
    I.atlas = atlas;
    I.img = img;
    I.scale = o->scale;
    I.ss = o->ss;
    I.shadows = o->shadows;
    memcpy(I.core, core, sizeof I.core);
    int ss = o->ss, nthreads = o->threads;
    for (int i = 0; i < 256; i++) I.lin[i] = srgb_to_lin(i / 255.0f);
    for (int i = 0; i < 4096; i++) I.enc[i] = (uint8_t)lrintf(lin_to_srgb(i / 4095.0f) * 255);
    I.Vn[0] = I.Vn[1] = I.Vn[2] = 0.57735027f;
    place_sun(&I.sun, o->sun);
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 256) nthreads = 256;

    /* the window's box: every corner projected, within the image */
    double cx = reg_x0 + MARGIN + REG / 2, cz = reg_z0 + MARGIN + REG / 2;
    double lo[2] = { 1e300, 1e300 }, hi[2] = { -1e300, -1e300 };
    size_t q0[MAT_COUNT + 1] = { 0 };
    for (int m = 0; m < MAT_COUNT; m++) {
        q0[m + 1] = q0[m] + mb[m].nv / 4;
        for (size_t i = 0; i < mb[m].nv; i++) {
            double sx, sy;
            float d;
            project(&I, cx, cz, &mb[m].pos[i * 3], &sx, &sy, &d);
            if (sx < lo[0]) lo[0] = sx;
            if (sx > hi[0]) hi[0] = sx;
            if (sy < lo[1]) lo[1] = sy;
            if (sy > hi[1]) hi[1] = sy;
        }
    }
    if (!q0[MAT_COUNT]) return 0;
    int px0 = (int)(floor(lo[0] / ss) - img->ox), px1 = (int)(ceil(hi[0] / ss) - img->ox);
    int py0 = (int)(floor(lo[1] / ss) - img->oy), py1 = (int)(ceil(hi[1] / ss) - img->oy);
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > img->w) px1 = img->w;
    if (py1 > img->h) py1 = img->h;
    if (px0 >= px1 || py0 >= py1) return 0;
    I.px0 = px0; I.py0 = py0;
    int ow = px1 - px0, oh = py1 - py0;
    I.W = ow * ss;
    I.H = oh * ss;
    double ox = (double)(img->ox + px0) * ss, oy = (double)(img->oy + py0) * ss;
    build_mip(&I);
    if (I.shadows) build_smap(&I, mb, nthreads);
    I.nq = q0[MAT_COUNT];
    I.q = xmalloc(I.nq * sizeof *I.q);
    I.ql = light_defer ? NULL : xmalloc(I.nq * sizeof *I.ql); /* else once it is known which show */
    pjob_t pj[256];
    pthread_t th[256];
    for (int i = 0; i < nthreads; i++) {
        pj[i] = (pjob_t){ .I = &I, .mb = mb, .ox = ox, .oy = oy, .cx = cx, .cz = cz,
                          .from = (int64_t)(I.nq * i / nthreads), .to = (int64_t)(I.nq * (i + 1) / nthreads) };
        memcpy(pj[i].q0, q0, sizeof q0);
        pthread_create(&th[i], NULL, prep_worker, &pj[i]);
    }
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);

    I.nbands = (oh + BAND - 1) / BAND;
    float *ylo = xmalloc(I.nq * sizeof *ylo), *yhi = xmalloc(I.nq * sizeof *yhi);
    for (size_t k = 0; k < I.nq; k++) {
        const iq_t *q = &I.q[k];
        ylo[k] = 1; yhi[k] = 0;
        if (q->mat == 255) continue;
        int32_t a = q->Y[0], b = q->Y[0];
        for (int i = 1; i < 4; i++) { if (q->Y[i] < a) a = q->Y[i]; if (q->Y[i] > b) b = q->Y[i]; }
        ylo[k] = (float)a / SUB / ss; yhi[k] = (float)b / SUB / ss;
    }
    bin_rows(I.nq, ylo, yhi, BAND, I.nbands, &I.bin, &I.bin0);
    free(ylo); free(yhi);
    sort_iso = &I;

    int per = nthreads;
    band_t *bands = calloc((size_t)per, sizeof *bands);
    size_t bn = (size_t)I.W * BAND * ss;
    for (int i = 0; i < per; i++) {
        bands[i].vis = xmalloc(bn * sizeof *bands[i].vis);
        bands[i].wid = xmalloc(bn * sizeof *bands[i].wid);
        bands[i].gid = xmalloc(bn * sizeof *bands[i].gid);
        bands[i].dep = xmalloc(bn * sizeof *bands[i].dep);
        bands[i].wdep = xmalloc(bn * sizeof *bands[i].wdep);
        bands[i].gdep = xmalloc(bn * sizeof *bands[i].gdep);
        bands[i].col = xmalloc(bn * 4 * sizeof *bands[i].col);
    }
    int nt = nthreads < I.nbands ? nthreads : I.nbands;
    if (light_defer) {
        /* which quads show, then their light */
        I.seen = calloc(I.nq, 1);
        if (!I.seen) { fprintf(stderr, "oom\n"); exit(1); }
        djob_t S = { .I = &I, .bands = bands, .first = 0, .count = I.nbands, .see = 1 };
        atomic_store(&S.next, 0);
        atomic_store(&S.worker, 0);
        for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, draw_worker, &S);
        for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
        /* light for them, and the glass, which is shaded wherever it is in front */
        uint32_t nl = 0;
        for (size_t k = 0; k < I.nq; k++)
            I.q[k].li = I.q[k].mat != 255 && (I.q[k].mat == MAT_BLEND || atomic_load_explicit(&I.seen[k], memory_order_relaxed)) ? nl++ : NO_LIGHT;
        free((void *)I.seen);
        I.ql = xmalloc((size_t)nl * sizeof *I.ql);
        static ljob_t L;
        L.p = pj[0];
        L.p.from = 0;
        L.p.to = (int64_t)I.nq;
        atomic_store(&L.next, 0);
        for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, light_worker, &L);
        for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    }
    /* the bands' rows are their own in the image: no two threads write one */
    djob_t J = { .I = &I, .bands = bands, .first = 0, .count = I.nbands };
    atomic_store(&J.next, 0);
    atomic_store(&J.worker, 0);
    for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, draw_worker, &J);
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < per; i++) {
        free(bands[i].vis); free(bands[i].wid); free(bands[i].gid); free(bands[i].dep); free(bands[i].wdep);
        free(bands[i].gdep); free(bands[i].col); free(bands[i].glass);
    }
    free(bands);
    free(I.q); free(I.ql); free(I.bin); free(I.bin0); free(I.mip);
    if (I.shadows) { free(I.sm.solid); free(I.sm.plant); free(I.sm.thin); }
    return 0;
}
