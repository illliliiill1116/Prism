/* Meshing: block models to quads, culled against their neighbours, lit
 * per corner, per level of detail (lod_mode), chunk by chunk on worker
 * threads and merged in tile order (mesh_pass). */
#include "prism.h"

int plant_far(int bx, int by, int bz) {
    int lo = by;
    while (lo > 0 && by - lo < 64 && states[gget(bx, lo - 1, bz)].up_lit) lo--;
    if (!(lo >= 1 && states[gget(bx, lo - 1, bz)].cull)) return PLANT_CANOPY;
    return states[gget(bx, by, bz)].fluid == FLUID_WATER ? PLANT_SUNK : PLANT_FADES;
}

/* level of detail being meshed: 0 full, 1 no plants and fast leaves,
 * 2 coarse cells (mesh_coarse) */
int lod_mode;

uint8_t mesh_skip[32 * 32];
int mesh_y0;

static void mb_reserve(mbuf_t *m, size_t addv, size_t addi) {
    if (m->nv + addv > m->cv) {
        m->cv = (m->cv ? m->cv * 2 : 4096);
        while (m->cv < m->nv + addv) m->cv *= 2;
        m->pos = realloc(m->pos, m->cv * 3 * sizeof(float));
        m->uv = realloc(m->uv, m->cv * 2 * sizeof(float));
        m->col = realloc(m->col, m->cv * VSTRIDE);
        /* light_defer (prism iso) has no use for par and idx, but dfr */
        if (light_defer) m->dfr = realloc(m->dfr, m->cv * DSTRIDE * sizeof(float));
        else m->par = realloc(m->par, m->cv * PSTRIDE);
        if (!m->pos || !m->uv || !m->col || !(light_defer ? (void *)m->dfr : (void *)m->par)) { fprintf(stderr, "oom\n"); exit(1); }
    }
    if (!light_defer && m->ni + addi > m->ci) {
        m->ci = (m->ci ? m->ci * 2 : 8192);
        while (m->ci < m->ni + addi) m->ci *= 2;
        m->idx = realloc(m->idx, m->ci * sizeof(uint32_t));
        if (!m->idx) { fprintf(stderr, "oom\n"); exit(1); }
    }
}

/* Corners in grid units (TL, TR, BR, BL as the texture sees them), uv in
 * 16ths, rotated by uvrot. Bakes each corner, as if facing up when up_lit,
 * as a leaf face (see LEAF_PASS) when leaf.
 * emit, the block light the block gives off, goes in COLOR_0's alpha (x 17)
 * for the viewer to make its bright texels glow. */
int light_defer;

/* A corner's light into its colours: COLOR_0, _SUN, _SUNWIN, _BLK, _BNC
 * and _BAMP in C, the light's parts in R (if any); lit as facing nl, a leaf face
 * if leaf, its tint T (sRGB). */
static void light_vertex(uint8_t *C, uint8_t *R, const float bp[3], const float nl[3], int leaf, const float T[3]) {
    float L[5][3], B[BOUNCE_ANGLES][3];
    uint8_t swin[4];
    int nwin = corner_light(bp, nl, leaf, L, swin, B);
    for (int c = 0; c < 3; c++) {
        float v2 = L[1][c] * T[c] * 0.5f, s2 = nwin ? (L[0][c] - L[1][c]) * T[c] * 0.5f : 0;
        C[c] = (uint8_t)(v2 >= 1 ? 255 : v2 * 255 + 0.5f);
        C[4 + c] = (uint8_t)(s2 >= 1 ? 255 : s2 <= 0 ? 0 : s2 * 255 + 0.5f);
        float b2 = L[2][c] * T[c] * 0.5f;
        C[12 + c] = (uint8_t)(b2 >= 1 ? 255 : b2 * 255 + 0.5f);
    }
    /* the bounce as a colour (_BNC rgb, its brightest channel BNC_Q) and
       how much of it per unit of the sun's height (_BAMP x, BAMP_Q x
       the square root of its share of BOUNCE_MAX), the mean over the
       angles: coarse, as it is soft, so it packs small */
    {
        float S[3] = { 0 }, m = 0, sy = 0;
        for (int k = 0; k < BOUNCE_ANGLES; k++) {
            sy += bounce_sun[k][1];
            for (int c = 0; c < 3; c++) S[c] += B[k][c] * T[c] * 0.5f;
        }
        for (int c = 0; c < 3; c++) m = (S[c] /= sy) > m ? S[c] : m;
        memset(C + 16, 0, 8);
        int a = (int)lrintf(BAMP_Q * sqrtf(m > BOUNCE_MAX ? 1 : m / BOUNCE_MAX));
        if (a >= BAMP_MIN) {
            C[20] = (uint8_t)a;
            for (int c = 0; c < 3; c++) C[16 + c] = (uint8_t)lrintf(S[c] / m * BNC_Q);
        }
    }
    if (!(C[4] | C[5] | C[6])) { swin[0] = 255; swin[1] = 0; swin[2] = 255; swin[3] = 0; }
    memcpy(C + 8, swin, 4);
    if (!R) return;
    float shade = face_shade(nl, leaf);
    R[0] = L[3][0] < 0 ? 255 : (uint8_t)L[3][0];
    R[1] = (uint8_t)lrintf(L[3][1] * 255);
    for (int c = 0; c < 3; c++) R[2 + c] = (uint8_t)lrintf(T[c] * 255);
    R[5] = (uint8_t)lrintf(shade * 255);
    R[6] = (uint8_t)lrintf(L[3][2] * 255);
    for (int c = 0; c < 3; c++) R[7 + c] = (uint8_t)lrintf(L[4][c] * 255);
    R[10] = R[11] = 0;
}

static const float UP[3] = { 0, 1, 0 };

void emit_quad(mbuf_t *m, float q[4][3], const canvas_t *tex, const float uv16[4],
                      int uvrot, const paint_t *pt, int up_lit, int leaf, int emit, long *nquads) {
    uint32_t si = tex_slot(tex);
    texslot_t *s = &slots[si];
    float e1[3], e2[3], n[3];
    for (int a = 0; a < 3; a++) { e1[a] = q[1][a] - q[0][a]; e2[a] = q[3][a] - q[0][a]; }
    n[0] = e2[1] * e1[2] - e2[2] * e1[1];
    n[1] = e2[2] * e1[0] - e2[0] * e1[2];
    n[2] = e2[0] * e1[1] - e2[1] * e1[0];
    float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (nl < 1e-9f) return;
    for (int a = 0; a < 3; a++) n[a] /= nl;

    /* the block the face belongs to: just behind its middle */
    int thin = 0;
    if (lod_mode != 2) {
        float c[3];
        for (int a = 0; a < 3; a++) c[a] = (q[0][a] + q[2][a]) * 0.5f - n[a] * 0.01f;
        int bx = (int)floorf(c[0]), by = (int)floorf(c[1]), bz = (int)floorf(c[2]);
        thin = bx >= 0 && bz >= 0 && bx < GW && bz < GW && by >= 0 && by < GH && !occ_at(bx, by, bz) && gget(bx, by, bz);
    }
    /* An upright plant on solid ground: its foot is shaded (PLANT_BASE).
     * One hanging from a block (cave vines, weeping vines: its column of
     * plants ends at an occluder above, not below) is marked (_BLK alpha)
     * for the viewer's wind to hold it by its top. */
    int footed = 0, foot_y = 0, hangs = 0, canopy = 0; /* canopy: plant_far draws it */
    if (up_lit) {
        /* the block behind the quad's centre (a far plant's face lies on its edge) */
        int bx = (int)floorf((q[0][0] + q[2][0]) * 0.5f - n[0] * 0.01f), by = (int)floorf((q[0][1] + q[2][1]) * 0.5f - n[1] * 0.01f),
            bz = (int)floorf((q[0][2] + q[2][2]) * 0.5f - n[2] * 0.01f);
        canopy = bx >= 0 && bz >= 0 && bx < GW && bz < GW && by >= 0 && by < GH && plant_far(bx, by, bz) != PLANT_FADES;
    }
    if (up_lit && fabsf(n[1]) < 0.5f && lod_mode != 2) {
        float c[3];
        for (int a = 0; a < 3; a++) c[a] = (q[0][a] + q[2][a]) * 0.5f;
        int bx = (int)floorf(c[0]), by = (int)floorf(c[1]), bz = (int)floorf(c[2]);
        if (bx >= 0 && bz >= 0 && bx < GW && bz < GW && by >= 0 && by < GH) {
            footed = by >= 1 && occ_at(bx, by - 1, bz);
            foot_y = by;
            int lo = by, hi = by;
            while (lo > 0 && by - lo < 64 && states[gget(bx, lo - 1, bz)].up_lit) lo--;
            while (hi + 1 < GH && hi - by < 64 && states[gget(bx, hi + 1, bz)].up_lit) hi++;
            hangs = !(lo >= 1 && occ_at(bx, lo - 1, bz)) && hi + 1 < GH && occ_at(bx, hi + 1, bz);
        }
    }
    const float cu[4] = { uv16[0], uv16[2], uv16[2], uv16[0] };
    const float cv[4] = { uv16[1], uv16[1], uv16[3], uv16[3] };
    int k = ((uvrot / 90) % 4 + 4) % 4;
    mb_reserve(m, 4, 6);
    uint32_t base = (uint32_t)m->nv;
    const float eps = 0.02f;
    for (int i = 0; i < 4; i++) {
        int j = (i - k + 4) % 4;
        float u = cu[j], v = cv[j];
        /* pull 1/50 texel towards the quad's uv centre so nearest sampling never hits a neighbour slot */
        float uc = (uv16[0] + uv16[2]) * 0.5f, vc = (uv16[1] + uv16[3]) * 0.5f;
        u += (u < uc ? eps : u > uc ? -eps : 0);
        v += (v < vc ? eps : v > vc ? -eps : 0);
        float *P = &m->pos[m->nv * 3];
        P[0] = q[i][0] - MARGIN - REG / 2;
        P[1] = q[i][1] + YMIN;
        P[2] = q[i][2] - MARGIN - REG / 2;
        float *U = &m->uv[m->nv * 2];
        U[0] = (s->x + s->w / 2 + u / 16.0f * s->w) / atlas_w;
        U[1] = (s->y + s->h / 2 + v / 16.0f * s->h) / atlas_h;
        float T[3] = { 1, 1, 1 };
        float bp[3] = { q[i][0], q[i][1], q[i][2] };
        if (lod_mode == 2)
            for (int k = 0; k < 6; k++) {
                int x = (int)floorf(bp[0] + n[0] * 0.5f), y = (int)floorf(bp[1] + n[1] * 0.5f), z = (int)floorf(bp[2] + n[2] * 0.5f);
                if (x < 0 || z < 0 || x >= GW || z >= GW || y < 0 || y >= GH || !occ_at(x, y, z)) break;
                for (int a = 0; a < 3; a++) bp[a] += n[a];
            }
        if (pt->kind == PAINT_FIXED) memcpy(T, pt->fixed, sizeof T);
        else if (pt->kind == PAINT_BIOME) biome_color_at(pt->bc, q[i], T);
        if (footed && q[i][1] < foot_y + 0.25f)
            for (int c = 0; c < 3; c++) T[c] *= PLANT_BASE;
        for (int c = 0; c < 3; c++) T[c] = lin_to_srgb(T[c]);
        uint8_t *C = &m->col[m->nv * VSTRIDE];
        if (light_defer) {
            /* unlit until light_quad: what it needs kept */
            memset(C, 0, VSTRIDE);
            float *D = &m->dfr[m->nv * DSTRIDE];
            memcpy(D, bp, sizeof bp);
            memcpy(D + 3, T, sizeof T);
            D[6] = (float)(up_lit | leaf << 1);
        } else
            light_vertex(C, &m->par[m->nv * PSTRIDE], bp, up_lit ? UP : n, leaf, T);
        C[15] = (uint8_t)(hangs | canopy << 1); /* the canopy bit goes to the tile's TC_PFLAG */
        C[3] = (uint8_t)(emit * 17);
        /* _SUN alpha 0 marks plants, which the viewer's map leaves unshaded,
         * LEAF_ALPHA leaves, which it lights through and leaves to the bake */
        C[7] = up_lit ? 0 : leaf ? LEAF_ALPHA : thin ? THIN_ALPHA : 255;
        m->nv++;
    }
    if (!light_defer) {
        uint32_t *I = &m->idx[m->ni];
        I[0] = base; I[1] = base + 3; I[2] = base + 1;
        I[3] = base + 1; I[4] = base + 3; I[5] = base + 2;
        m->ni += 6;
    }
    (*nquads)++;
}

void light_quad(mbuf_t *m, size_t lq) {
    const float *D = &m->dfr[lq * 4 * DSTRIDE];
    /* the normal as emit_quad had it, from the same corners */
    float e1[3], e2[3], n[3];
    for (int a = 0; a < 3; a++) { e1[a] = D[DSTRIDE + a] - D[a]; e2[a] = D[3 * DSTRIDE + a] - D[a]; }
    n[0] = e2[1] * e1[2] - e2[2] * e1[1];
    n[1] = e2[2] * e1[0] - e2[0] * e1[2];
    n[2] = e2[0] * e1[1] - e2[1] * e1[0];
    float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int a = 0; a < 3; a++) n[a] /= nl;
    for (int i = 0; i < 4; i++) {
        const float *d = &D[i * DSTRIDE];
        int f = (int)d[6];
        size_t v = lq * 4 + (size_t)i;
        light_vertex(&m->col[v * VSTRIDE], m->par ? &m->par[v * PSTRIDE] : NULL, d, f & 1 ? UP : n, f >> 1, d + 3);
    }
}

static int mat_for(const canvas_t *t) {
    uint32_t i = tex_slot(t);
    const texslot_t *s = &slots[i];
    return s->has_partial ? MAT_BLEND : s->has_zero ? MAT_MASK : MAT_OPAQUE;
}

/* Box face corners, TL TR BR BL, in block units relative to the block. */
void box_face(int f, const float b[6], float q[4][3]) {
    float x0 = b[0], y0 = b[1], z0 = b[2], x1 = b[3], y1 = b[4], z1 = b[5];
    switch (f) {
    case FACE_UP:    { float c[4][3] = { { x0, y1, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { x0, y1, z1 } }; memcpy(q, c, sizeof c); break; }
    case FACE_DOWN:  { float c[4][3] = { { x0, y0, z1 }, { x1, y0, z1 }, { x1, y0, z0 }, { x0, y0, z0 } }; memcpy(q, c, sizeof c); break; }
    case FACE_NORTH: { float c[4][3] = { { x1, y1, z0 }, { x0, y1, z0 }, { x0, y0, z0 }, { x1, y0, z0 } }; memcpy(q, c, sizeof c); break; }
    case FACE_SOUTH: { float c[4][3] = { { x0, y1, z1 }, { x1, y1, z1 }, { x1, y0, z1 }, { x0, y0, z1 } }; memcpy(q, c, sizeof c); break; }
    case FACE_EAST:  { float c[4][3] = { { x1, y1, z1 }, { x1, y1, z0 }, { x1, y0, z0 }, { x1, y0, z1 } }; memcpy(q, c, sizeof c); break; }
    default:         { float c[4][3] = { { x0, y1, z0 }, { x0, y1, z1 }, { x0, y0, z1 }, { x0, y0, z0 } }; memcpy(q, c, sizeof c); break; }
    }
}

/* A leaf block with leaves or solid blocks on all six sides: nothing
 * behind it shows through the canopy's outer layer. */
static int leaf_buried(int gx, int gy, int gz) {
    for (int f = 0; f < FACE_COUNT; f++) {
        const state_t *s = &states[gget(gx + DIRV[f][0], gy + DIRV[f][1], gz + DIRV[f][2])];
        if (!s->cull && !s->leaves) return 0;
    }
    return 1;
}

static int face_hidden(uint16_t self, int gx, int gy, int gz, int f) {
    int nx = gx + DIRV[f][0], ny = gy + DIRV[f][1], nz = gz + DIRV[f][2];
    uint16_t nb = gget(nx, ny, nz);
    if (states[nb].cull) return 1;
    if (states[self].self_cull && states[nb].name_hash == states[self].name_hash) return 1;
    /* leaves are cut out, so faces between leaves show through the gaps;
     * only those against a buried leaf are dropped */
    if (states[self].leaves && states[nb].leaves && (lod_mode || leaf_buried(nx, ny, nz))) return 1;
    if (states[self].leaves && states[nb].up_lit && lod_mode == 1) return 1; /* a leaf block there too (mesh_far_plant) */
    return 0;
}

/* The game's BlockBehaviour.OffsetType.XYZ: up to 1/4 block across and 0.2
 * down, from Mth.getSeed(x, 0, z) of the world position. */
static void plant_offset(int gx, int gz, float o[3]) {
    int32_t x = reg_x0 + gx, z = reg_z0 + gz;
    int64_t l = (int64_t)(int32_t)((uint32_t)x * 3129871u) ^ (int64_t)((uint64_t)(int64_t)z * 116129781u);
    l = (int64_t)((uint64_t)l * (uint64_t)l * 42317861u + (uint64_t)l * 11u) >> 16;
    o[0] = (float)((((float)(l & 15) / 15.0f) - 0.5) * 0.5);
    o[1] = ((float)((l >> 4) & 15) / 15.0f - 1.0f) * 0.2f;
    o[2] = (float)((((float)((l >> 8) & 15) / 15.0f) - 0.5) * 0.5);
}

/* p' = xf * (p, 1), xf in 16ths */
static void xform_pts(float q[4][3], const double xf[3][4]) {
    for (int i = 0; i < 4; i++) {
        double p[3] = { q[i][0] * 16.0, q[i][1] * 16.0, q[i][2] * 16.0 };
        for (int a = 0; a < 3; a++) q[i][a] = (float)((xf[a][0] * p[0] + xf[a][1] * p[1] + xf[a][2] * p[2] + xf[a][3]) / 16.0);
    }
}

static void rotate_pts(float q[4][3], int axis, double angle_deg, const double org[3], int rescale) {
    double a = angle_deg * M_PI / 180.0, c = cos(a), s = sin(a);
    double k = rescale ? 1 / fabs(c) : 1;
    for (int i = 0; i < 4; i++) {
        double p[3] = { q[i][0] - org[0] / 16.0, q[i][1] - org[1] / 16.0, q[i][2] - org[2] / 16.0 };
        double r[3] = { p[0], p[1], p[2] };
        if (axis == 0) { r[1] = (p[1] * c - p[2] * s) * k; r[2] = (p[1] * s + p[2] * c) * k; }
        else if (axis == 1) { r[0] = (p[0] * c + p[2] * s) * k; r[2] = (-p[0] * s + p[2] * c) * k; }
        else { r[0] = (p[0] * c - p[1] * s) * k; r[1] = (p[0] * s + p[1] * c) * k; }
        q[i][0] = (float)(r[0] + org[0] / 16.0);
        q[i][1] = (float)(r[1] + org[1] / 16.0);
        q[i][2] = (float)(r[2] + org[2] / 16.0);
    }
}

/* Where each chunk's quads landed: its worker and, per material, the span
 * of that worker's buffers. The merge regroups them by tile from this. */
typedef struct { size_t v0, nv, i0, ni; } span_t;

static worker_t *chunk_owner[32 * 32];

static span_t chunk_span[32 * 32][MAT_COUNT];

/* Surface height at the corner of (gx, gz) towards (dx, dz), the way the
 * game smooths fluids: 1 if any of the four columns sharing the corner has
 * the fluid above, else a weighted mean where near-full cells count ten
 * times, open cells pull the corner down and solid cells are ignored.
 * Neighbours share their corners, so slopes have no gaps. */
static float fluid_corner(int fluid, int gx, int gy, int gz, int dx, int dz) {
    float sum = 0, wsum = 0;
    for (int k = 0; k < 4; k++) {
        int x = gx + ((k & 1) ? dx : 0), z = gz + ((k & 2) ? dz : 0);
        const state_t *c = &states[gget(x, gy, z)];
        if (c->fluid == fluid) {
            if (states[gget(x, gy + 1, z)].fluid == fluid) return 1.0f;
            float h = c->fluid_h / 16.0f, wt = h >= 0.8f ? 10.0f : 1.0f;
            sum += h * wt;
            wsum += wt;
        } else if (!c->cull) {
            wsum += 1;
        }
    }
    return wsum > 0 ? sum / wsum : 0;
}

static void mesh_block_inner(worker_t *w, int gx, int gy, int gz, uint16_t id);

static void mesh_block(worker_t *w, int gx, int gy, int gz) {
    mesh_block_inner(w, gx, gy, gz, grid[gidx(gx, gy, gz)]);
}

/* how a face of st is coloured: its own tint, the state's, or the biome's */
static void face_paint(const state_t *st, const blockmodel_shape_face_t *fc, paint_t *pt) {
    pt->kind = PAINT_NONE;
    if (fc->has_tint_rgb) {
        pt->kind = PAINT_FIXED;
        rgb8_lin(fc->tint_rgb, pt->fixed);
    } else if (fc->tint && st->tint_kind == TINT_FIXED) {
        pt->kind = PAINT_FIXED;
        rgb8_lin(st->tint_fixed, pt->fixed);
    } else if (fc->tint && st->tint_kind != TINT_NONE) {
        pt->kind = PAINT_BIOME;
        pt->bc = st->tint_kind == TINT_GRASS ? BC_GRASS : st->tint_kind == TINT_FOLIAGE ? BC_FOLIAGE : BC_DRY;
    }
}

/* Level 1's plant (plant_far): a solid block of the plant's mean colour,
 * lit as the plant is (up_lit) and so marked to stay. Solid, a clump
 * (of plants and leaves) is only its shell, hides what is behind it
 * before it is shaded, and needs no cut-out test: a cut-out block in the
 * plant's texture looked the same and cost as much as the crosses. */
/* Through a sunk plant's gaps shows the dark water behind: a solid block
 * for it takes that share at FAR_PLANT_BEHIND of its colour, or a kelp
 * forest read brighter than the crosses it stands for. (A canopy's gaps
 * show more lit foliage: it keeps its colour.) */
#define FAR_PLANT_BEHIND 0.8f

float far_plant_dim(const state_t *st) {
    for (uint32_t ei = 0; ei < st->shape->element_count; ei++)
        for (int f = 0; f < FACE_COUNT; f++)
            if (st->shape->elements[ei].faces[f].texture)
                return FAR_PLANT_BEHIND + (1 - FAR_PLANT_BEHIND) * slots[tex_slot(st->shape->elements[ei].faces[f].texture)].cover;
    return 1;
}

static int far_face_open(int x, int y, int z, int f) {
    int nx = x + DIRV[f][0], ny = y + DIRV[f][1], nz = z + DIRV[f][2];
    const state_t *nb = &states[gget(nx, ny, nz)];
    return !(nb->cull || nb->up_lit || nb->leaves || is_cave(nx, ny, nz));
}

/* A sunk plant's far form stops this far under the water's top (14/16 of
 * its top block): the crosses it stands for show mostly below that, seen
 * through the water, and a top just under the surface took none of it -
 * a kelp forest's tops read as lying on the sea. */
#define SUNK_TOP_DEPTH 1.5f

/* the height a sunk plant at x, y, z is cut off at (sunk_cap) */
float sunk_cap(int x, int y, int z) {
    int sy = y;
    while (sy + 1 < GH && states[gget(x, sy + 1, z)].fluid == FLUID_WATER) sy++;
    return sy + 14 / 16.0f - SUNK_TOP_DEPTH;
}

static void mesh_far_plant(worker_t *w, int gx, int gy, int gz, const state_t *st) {
    int far = plant_far(gx, gy, gz);
    if (far == PLANT_FADES) return;
    float cap = far == PLANT_SUNK ? sunk_cap(gx, gy, gz) : (float)GH;
    if (cap <= gy) return;
    uint16_t id = gget(gx, gy, gz);
    const float full_uv[4] = { 0, 0, 16, 16 };
    const float dim = far == PLANT_SUNK ? far_plant_dim(st) : 1;
    for (int f = 0; f < FACE_COUNT; f++) {
        /* the top where the cap cuts the column, whatever is above */
        if (!far_face_open(gx, gy, gz, f) && !(f == FACE_UP && cap < gy + 1)) continue;
        /* the same plant's sides one above another are one quad (a kelp
           stalk's: one colour, so nothing to tile), from the lowest */
        int h = 1;
        if (f != FACE_UP && f != FACE_DOWN) {
            if (gy > 0 && gget(gx, gy - 1, gz) == id && far_face_open(gx, gy - 1, gz, f)) continue;
            while (gy + h < GH && h < 64 && gy + h < cap && gget(gx, gy + h, gz) == id && far_face_open(gx, gy + h, gz, f)) h++;
        }
        float top = gy + h < cap ? (float)(gy + h) : cap;
        const float box[6] = { (float)gx, (float)gy, (float)gz, (float)gx + 1, top, (float)gz + 1 };
        float q[4][3];
        box_face(f, box, q);
        paint_t pt = { PAINT_FIXED, 0, { st->face_col[f][0], st->face_col[f][1], st->face_col[f][2] } };
        if (st->face_bc[f] >= 0) {
            float p[3] = { gx + 0.5f, gy + 0.5f, gz + 0.5f }, t[3];
            biome_color_at(st->face_bc[f], p, t);
            for (int a = 0; a < 3; a++) pt.fixed[a] *= t[a];
        }
        for (int a = 0; a < 3; a++) pt.fixed[a] *= dim;
        emit_quad(&w->mb[MAT_OPAQUE], q, tex_white, full_uv, 0, &pt, 1, 0, st->emit, &w->quads);
    }
}

static void mesh_block_inner(worker_t *w, int gx, int gy, int gz, uint16_t id) {
    const state_t *st = &states[id];
    if (st->air) return;
    paint_t pt;

    if (st->fluid) {
        int water = st->fluid == FLUID_WATER;
        const canvas_t *tex = water ? tex_water : tex_lava;
        uint16_t up = gget(gx, gy + 1, gz);
        int up_same = states[up].fluid == st->fluid;
        float ch[2][2]; /* surface height per corner, [x][z] */
        for (int cx = 0; cx < 2; cx++)
            for (int cz = 0; cz < 2; cz++)
                ch[cx][cz] = up_same ? 1.0f : fluid_corner(st->fluid, gx, gy, gz, cx ? 1 : -1, cz ? 1 : -1);
        pt = (paint_t){ water ? PAINT_BIOME : PAINT_NONE, BC_WATER, { 1, 1, 1 } };
        const float box[6] = { 0, 0, 0, 1, 1, 1 };
        const float full_uv[4] = { 0, 0, 16, 16 };
        for (int f = 0; f < FACE_COUNT; f++) {
            uint16_t nb = gget(gx + DIRV[f][0], gy + DIRV[f][1], gz + DIRV[f][2]);
            if (f == FACE_UP) { if (up_same) continue; }
            else if (states[nb].fluid == st->fluid || states[nb].cull) continue;
            if (is_cave(gx + DIRV[f][0], gy + DIRV[f][1], gz + DIRV[f][2])) continue;
            float q[4][3];
            box_face(f, box, q);
            for (int i = 0; i < 4; i++) {
                if (q[i][1] > 0) q[i][1] = ch[(int)q[i][0]][(int)q[i][2]];
                q[i][0] += gx; q[i][1] += gy; q[i][2] += gz;
            }
            emit_quad(&w->mb[water ? MAT_WATER : MAT_OPAQUE], q, tex, full_uv, 0, &pt, 0, 0, 0, &w->quads);
        }
    }
    if (!st->shape) return;
    /* a plant's own water (kelp, seagrass) is drawn at every level */
    if (lod_mode && st->up_lit) {
        if (lod_mode == 1) mesh_far_plant(w, gx, gy, gz, st);
        return;
    }
    const canvas_t *own = st->be_tex ? be_texture(gx, gy, gz) : NULL;
    for (uint32_t ei = 0; ei < st->shape->element_count; ei++) {
        const blockmodel_element_t *e = &st->shape->elements[ei];
        float box[6] = { (float)(e->x0 / 16), (float)(e->y0 / 16), (float)(e->z0 / 16),
                         (float)(e->x1 / 16), (float)(e->y1 / 16), (float)(e->z1 / 16) };
        for (int f = 0; f < FACE_COUNT; f++) {
            const blockmodel_shape_face_t *fc = &e->faces[f];
            if (!fc->texture) continue;
            int on_edge = (f == FACE_UP && e->y1 >= 16) || (f == FACE_DOWN && e->y0 <= 0) ||
                          (f == FACE_NORTH && e->z0 <= 0) || (f == FACE_SOUTH && e->z1 >= 16) ||
                          (f == FACE_EAST && e->x1 >= 16) || (f == FACE_WEST && e->x0 <= 0);
            if (on_edge && e->rot_angle == 0 && !e->has_xf && face_hidden(id, gx, gy, gz, f)) continue;
            if (on_edge ? is_cave(gx + DIRV[f][0], gy + DIRV[f][1], gz + DIRV[f][2]) : is_cave(gx, gy, gz)) continue;
            float q[4][3];
            box_face(f, box, q);
            if (e->rot_angle != 0) rotate_pts(q, e->rot_axis, e->rot_angle, e->rot_origin, e->rot_rescale);
            if (e->has_xf) xform_pts(q, e->xf);
            float jo[3] = { 0, 0, 0 };
            if (st->jitter) plant_offset(gx, gz, jo);
            for (int i = 0; i < 4; i++) { q[i][0] += gx + jo[0]; q[i][1] += gy + jo[1]; q[i][2] += gz + jo[2]; }
            pt.kind = PAINT_NONE;
            if (!own) face_paint(st, fc, &pt); /* else already coloured (a banner's dyes) */
            float uv16[4] = { fc->u0, fc->v0, fc->u1, fc->v1 };
            const canvas_t *tex = own ? own : fc->texture;
            emit_quad(&w->mb[mat_for(tex)], q, tex, uv16, fc->uvrot, &pt, st->up_lit, st->leaves, st->emit, &w->quads);
        }
    }
}

static atomic_int next_chunk;

static void *mesh_worker(void *arg) {
    worker_t *w = arg;
    for (;;) {
        int c = atomic_fetch_add(&next_chunk, 1);
        if (c >= 32 * 32) break;
        int cx = c % 32, cz = c / 32;
        span_t *sp = chunk_span[c];
        for (int m = 0; m < MAT_COUNT; m++) { sp[m].v0 = w->mb[m].nv; sp[m].i0 = w->mb[m].ni; }
        if (mesh_skip[c]) ;
        else if (lod_mode == 2) mesh_coarse(w, cx, cz);
        else
            for (int gy = mesh_y0; gy < GH; gy++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++)
                        mesh_block(w, MARGIN + cx * 16 + x, gy, MARGIN + cz * 16 + z);
        for (int m = 0; m < MAT_COUNT; m++) { sp[m].nv = w->mb[m].nv - sp[m].v0; sp[m].ni = w->mb[m].ni - sp[m].i0; }
        chunk_owner[c] = w;
    }
    return NULL;
}

/* Meshes the region's chunks at the current lod_mode on nthreads and
 * merges them chunk by chunk in tile order, whichever worker meshed it:
 * the result is grouped by tile and the same for any thread count. */
long mesh_pass(int nthreads, mbuf_t merged[MAT_COUNT], size_t tile_q0[MAT_COUNT][NTILES + 1]) {
    pthread_t th[256];
    worker_t *ws = calloc((size_t)nthreads, sizeof *ws);
    atomic_store(&next_chunk, 0);
    for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, mesh_worker, &ws[i]);
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    memset(merged, 0, MAT_COUNT * sizeof *merged);
    /* the chunks in tile order */
    int order[32 * 32], no = 0;
    const int TW = 32 / TILE_CHUNKS;
    for (int t = 0; t < NTILES; t++)
        for (int k = 0; k < TILE_CHUNKS * TILE_CHUNKS; k++)
            order[no++] = (t / TW * TILE_CHUNKS + k / TILE_CHUNKS) * 32 + t % TW * TILE_CHUNKS + k % TILE_CHUNKS;
    for (int m = 0; m < MAT_COUNT; m++) {
        mbuf_t *d = &merged[m];
        size_t nv = 0, ni = 0;
        for (int i = 0; i < nthreads; i++) { nv += ws[i].mb[m].nv; ni += ws[i].mb[m].ni; }
        for (int t = 0, at = 0; t < NTILES; t++) {
            tile_q0[m][t] = (size_t)at / 4;
            for (int k = 0; k < TILE_CHUNKS * TILE_CHUNKS; k++) at += (int)chunk_span[order[t * TILE_CHUNKS * TILE_CHUNKS + k]][m].nv;
        }
        tile_q0[m][NTILES] = nv / 4;
        /* an array at a time, each worker's freed once copied: the mesh is
         * held twice over only one array's worth */
#define MERGE(f, sz)                                                                                   \
        if (ws[0].mb[m].f || nv) {                                                                      \
            d->f = xmalloc(nv * (sz) + 1);                                                              \
            size_t at = 0;                                                                              \
            for (int o = 0; o < no; o++) {                                                              \
                const span_t *sp = &chunk_span[order[o]][m];                                            \
                memcpy((char *)d->f + at * (sz), (const char *)chunk_owner[order[o]]->mb[m].f + sp->v0 * (sz), sp->nv * (sz)); \
                at += sp->nv;                                                                           \
            }                                                                                           \
            for (int i = 0; i < nthreads; i++) { free(ws[i].mb[m].f); ws[i].mb[m].f = NULL; }           \
        }
        MERGE(pos, 12)
        MERGE(uv, 8)
        MERGE(col, VSTRIDE)
        if (light_defer) { MERGE(dfr, DSTRIDE * sizeof(float)) }
        else { MERGE(par, PSTRIDE) }
#undef MERGE
        if (!light_defer) {
            d->idx = xmalloc(ni * sizeof *d->idx + 1);
            size_t vat = 0, iat = 0;
            for (int o = 0; o < no; o++) {
                const span_t *sp = &chunk_span[order[o]][m];
                const mbuf_t *s = &chunk_owner[order[o]]->mb[m];
                for (size_t j = 0; j < sp->ni; j++) d->idx[iat + j] = s->idx[sp->i0 + j] - (uint32_t)sp->v0 + (uint32_t)vat;
                vat += sp->nv; iat += sp->ni;
            }
        }
        d->nv = d->cv = nv;
        d->ni = d->ci = ni;
    }
    long quads = 0;
    for (int i = 0; i < nthreads; i++) {
        quads += ws[i].quads;
        for (int m = 0; m < MAT_COUNT; m++) {
            free(ws[i].mb[m].pos); free(ws[i].mb[m].uv); free(ws[i].mb[m].col); free(ws[i].mb[m].par); free(ws[i].mb[m].idx);
            free(ws[i].mb[m].dfr);
        }
    }
    free(ws);
    return quads;
}

void mb_free(mbuf_t mb[MAT_COUNT]) {
    for (int m = 0; m < MAT_COUNT; m++) {
        free(mb[m].pos); free(mb[m].uv); free(mb[m].col); free(mb[m].par); free(mb[m].idx); free(mb[m].dfr);
    }
}
