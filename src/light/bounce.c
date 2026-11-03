/* Bounced sunlight: what the lit faces around a corner reflect onto it. */
#include "prism.h"

/* One bounce of sunlight: what the lit faces around a corner reflect onto
 * it, worked out at BOUNCE_ANGLES points of the sun's arc and kept as their
 * mean per unit of the sun's height (the viewer scales it by the sun's). BOUNCE_RAYS cosine-weighted rays go out to BOUNCE_RANGE;
 * a face one hits is lit as a whole, from its middle (every blocker, near
 * ones too, and leaves as solid: the light let through a canopy is the
 * viewer's to add, and counting its inner faces as lit made everything in
 * a crown glow), and reflects its mean texture colour (face_col, linear). Only
 * the sun's light bounces: the sky's stays SKY_FLOOR's flat share, which
 * is low for it. */
#define BOUNCE_RAYS 48

#define BOUNCE_RANGE 16

#define BOUNCE_GAIN 4.0f

static float bounce_dirs[BOUNCE_RAYS][3]; /* cosine-weighted around +z */

int bounce_rays = BOUNCE_RAYS;

float bounce_sun[BOUNCE_ANGLES][3];

void setup_bounce(void) {
    const float golden = 2.39996323f;
    for (int i = 0; i < bounce_rays; i++) {
        float u = (i + 0.5f) / bounce_rays, r = sqrtf(u), phi = i * golden;
        bounce_dirs[i][0] = r * cosf(phi);
        bounce_dirs[i][1] = r * sinf(phi);
        bounce_dirs[i][2] = sqrtf(1 - u);
    }
    for (int k = 0; k < BOUNCE_ANGLES; k++) sun_at((k + 0.5f) * 3.14159265f / BOUNCE_ANGLES, bounce_sun[k]);
}

void set_bounce_rays(int n) {
    bounce_rays = n < 1 ? 1 : n > BOUNCE_RAYS ? BOUNCE_RAYS : n;
    setup_bounce();
}

#define FC_PROBE 32

fcache_t *fcache;
int fc_bits = FC_BITS;

static void face_light(int x, int y, int z, int f, float col[3], uint8_t e[BOUNCE_ANGLES]) {
    const state_t *st = &states[gget(x, y, z)];
    const int *dv = DIRV[f];
    float c[3] = { x + 0.5f + dv[0] * 0.6f, y + 0.5f + dv[1] * 0.6f, z + 0.5f + dv[2] * 0.6f };
    memcpy(col, st->face_col[f], sizeof st->face_col[f]);
    if (st->face_bc[f] >= 0) {
        float t[3];
        biome_color_at(st->face_bc[f], c, t);
        for (int a = 0; a < 3; a++) col[a] *= t[a];
    }
    for (int k = 0; k < BOUNCE_ANGLES; k++) {
        const float *d = bounce_sun[k];
        float cs = dv[0] * d[0] + dv[1] * d[1] + dv[2] * d[2];
        e[k] = cs > 0 && ray_free(c, d, 200, 0) ? (uint8_t)lrintf(cs * 255) : 0;
    }
}

static void face_light_cached(int x, int y, int z, int f, float col[3], uint8_t e[BOUNCE_ANGLES]) {
    uint32_t key = (uint32_t)(gidx(x, y, z) * 6 + (size_t)f), h = key * 0x9e3779b1u;
    h ^= h >> 15;
    fcache_t *slot = NULL;
    for (int i = 0; fcache && i < FC_PROBE; i++) {
        fcache_t *s = &fcache[(h + (uint32_t)i) & ((1u << fc_bits) - 1)];
        uint32_t st = atomic_load_explicit(&s->state, memory_order_acquire);
        if (st == 2) {
            if (s->key != key) continue;
            memcpy(col, s->col, sizeof s->col);
            memcpy(e, s->e, sizeof s->e);
            return;
        }
        if (st == 1) break;
        uint32_t want = 0;
        if (atomic_compare_exchange_strong_explicit(&s->state, &want, 1, memory_order_acquire, memory_order_relaxed)) { slot = s; break; }
        if (want == 2 && s->key == key) {
            memcpy(col, s->col, sizeof s->col);
            memcpy(e, s->e, sizeof s->e);
            return;
        }
        if (want == 1) break;
    }
    face_light(x, y, z, f, col, e);
    if (slot) {
        slot->key = key;
        memcpy(slot->col, col, sizeof slot->col);
        memcpy(slot->e, e, sizeof slot->e);
        atomic_store_explicit(&slot->state, 2, memory_order_release);
    }
}

/* The face a ray going along axis a by step s enters a voxel through */
static int entry_face(int a, int s) {
    return a == 0 ? (s > 0 ? FACE_WEST : FACE_EAST) : a == 1 ? (s > 0 ? FACE_DOWN : FACE_UP) : (s > 0 ? FACE_NORTH : FACE_SOUTH);
}

/* The bounced sun at p facing n, B[k] for the sun at bounce_sun[k] (a full
 * sun, as bake()'s out has it, before shade and tint). */
void bounce(const float p[3], const float n[3], float B[BOUNCE_ANGLES][3]) {
    memset(B, 0, sizeof(float) * BOUNCE_ANGLES * 3);
    const float a[3] = { fabsf(n[0]) < 0.9f ? 1.0f : 0.0f, fabsf(n[0]) < 0.9f ? 0.0f : 1.0f, 0 };
    float t[3] = { a[1] * n[2] - a[2] * n[1], a[2] * n[0] - a[0] * n[2], a[0] * n[1] - a[1] * n[0] }, b[3];
    normalize3(t);
    b[0] = n[1] * t[2] - n[2] * t[1];
    b[1] = n[2] * t[0] - n[0] * t[2];
    b[2] = n[0] * t[1] - n[1] * t[0];
    const float o[3] = { p[0] + n[0] * 0.3f, p[1] + n[1] * 0.3f, p[2] + n[2] * 0.3f };
    /* the highest occluder in reach: a ray not going down is out past it */
    int top = -1;
    {
        int bx0 = (int)floorf(o[0]) - BOUNCE_RANGE, bx1 = (int)floorf(o[0]) + BOUNCE_RANGE;
        int bz0 = (int)floorf(o[2]) - BOUNCE_RANGE, bz1 = (int)floorf(o[2]) + BOUNCE_RANGE;
        bx0 = bx0 < 0 ? 0 : bx0; bz0 = bz0 < 0 ? 0 : bz0;
        bx1 = bx1 >= GW ? GW - 1 : bx1; bz1 = bz1 >= GW ? GW - 1 : bz1;
        for (int z = bz0 / HM_CELL; z <= bz1 / HM_CELL; z++)
            for (int x = bx0 / HM_CELL; x <= bx1 / HM_CELL; x++)
                if (hm_top[z * HM_W + x] > top) top = hm_top[z * HM_W + x];
    }
    for (int r = 0; r < bounce_rays; r++) {
        const float *l = bounce_dirs[r];
        float d[3];
        for (int i = 0; i < 3; i++) d[i] = t[i] * l[0] + b[i] * l[1] + n[i] * l[2];
        int ip[3] = { (int)floorf(o[0]), (int)floorf(o[1]), (int)floorf(o[2]) }, step[3];
        float tmax[3], tdel[3];
        for (int i = 0; i < 3; i++) {
            if (d[i] > 1e-7f) { step[i] = 1; tdel[i] = 1 / d[i]; tmax[i] = (ip[i] + 1 - o[i]) / d[i]; }
            else if (d[i] < -1e-7f) { step[i] = -1; tdel[i] = -1 / d[i]; tmax[i] = (o[i] - ip[i]) / -d[i]; }
            else { step[i] = 0; tdel[i] = 1e30f; tmax[i] = 1e30f; }
        }
        for (;;) {
            int ax = tmax[0] < tmax[1] ? (tmax[0] < tmax[2] ? 0 : 2) : (tmax[1] < tmax[2] ? 1 : 2);
            if (tmax[ax] > BOUNCE_RANGE) break;
            float tin = tmax[ax];
            ip[ax] += step[ax];
            tmax[ax] += tdel[ax];
            if (ip[0] < 0 || ip[2] < 0 || ip[0] >= GW || ip[2] >= GW || ip[1] < 0 || ip[1] >= GH) break;
            if (ip[1] > top && step[1] >= 0) break;
            if (!occ_at(ip[0], ip[1], ip[2]) || occ_miss(ip[0], ip[1], ip[2], o, d, tin, fminf(fminf(tmax[0], tmax[1]), tmax[2]))) continue;
            float col[3];
            uint8_t e[BOUNCE_ANGLES];
            face_light_cached(ip[0], ip[1], ip[2], entry_face(ax, step[ax]), col, e);
            for (int k = 0; k < BOUNCE_ANGLES; k++)
                for (int c = 0; c < 3; c++) B[k][c] += col[c] * e[k];
            break;
        }
    }
    for (int k = 0; k < BOUNCE_ANGLES; k++)
        for (int c = 0; c < 3; c++) B[k][c] *= SUN_COL[c] * BOUNCE_GAIN / (255.0f * bounce_rays);
}
