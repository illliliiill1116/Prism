/* Level 2: the region as CK x 1 x CK cells painted with their blocks' mean
 * colours. */
#include "prism.h"

#define CK 4 /* a cell is CK x 1 x CK blocks */

/* A block a coarse cell takes as solid: what stops light, or a plant
 * level 1 draws (mesh_far_plant), below its cut off (sunk_cap). */
static inline int coarse_solid(int x, int y, int z) {
    if (occ_at(x, y, z)) return 1;
    const state_t *st = &states[gget(x, y, z)];
    if (!st->up_lit) return 0;
    int far = plant_far(x, y, z);
    return far == PLANT_CANOPY || (far == PLANT_SUNK && y + 1 <= sunk_cap(x, y, z));
}

/* What a block contributes to a cell: solid (coarse_solid, or a cave), water, or neither. */
static inline int fine_kind(int x, int y, int z) {
    if (y < 0) return 1;
    if (y >= GH) return 0;
    if (coarse_solid(x, y, z) || is_cave(x, y, z)) return 1;
    return states[gget(x, y, z)].fluid == FLUID_WATER ? 2 : 0;
}

/* Fills each state's per-face mean colour: the largest textured face
 * pointing that way, else the largest face at all. */
void prepare_face_colors(void) {
    for (uint32_t i = 0; i < nstates; i++) {
        state_t *st = &states[i];
        for (int f = 0; f < FACE_COUNT; f++) {
            st->face_bc[f] = -1;
            for (int c = 0; c < 3; c++) st->face_col[f][c] = 0.5f;
        }
        if (!st->shape || !st->shape->element_count) continue;
        for (int f = 0; f < FACE_COUNT; f++) {
            double best = -1;
            for (int pass = 0; pass < 2 && best < 0; pass++)
                for (uint32_t ei = 0; ei < st->shape->element_count; ei++) {
                    const blockmodel_element_t *e = &st->shape->elements[ei];
                    for (int g = 0; g < FACE_COUNT; g++) {
                        if ((pass == 0) != (g == f)) continue;
                        const blockmodel_shape_face_t *fc = &e->faces[g];
                        if (!fc->texture) continue;
                        double dx = e->x1 - e->x0, dy = e->y1 - e->y0, dz = e->z1 - e->z0;
                        double area = g < 2 ? dx * dz : g < 4 ? dx * dy : dy * dz;
                        if (area <= best) continue;
                        best = area;
                        uint32_t si = tex_slot(fc->texture);
                        const float *m = slots[si].mean;
                        float t[3] = { 1, 1, 1 };
                        st->face_bc[f] = -1;
                        if (fc->has_tint_rgb) rgb8_lin(fc->tint_rgb, t);
                        else if (fc->tint && st->tint_kind == TINT_FIXED) rgb8_lin(st->tint_fixed, t);
                        else if (fc->tint && st->tint_kind != TINT_NONE)
                            st->face_bc[f] = (int8_t)(st->tint_kind == TINT_GRASS ? BC_GRASS : st->tint_kind == TINT_FOLIAGE ? BC_FOLIAGE : BC_DRY);
                        for (int c = 0; c < 3; c++) st->face_col[f][c] = m[c] * t[c];
                    }
                }
        }
    }
}

/* One chunk's cells: solid where a quarter of the blocks are (at half,
 * sparse canopies - spruce, the edges of any crown - fell through and far
 * forests turned to bare ground), water where half are water. A face's colour is the mean of the blocks behind
 * it, those open towards it if any. */
void mesh_coarse(worker_t *w, int cx, int cz) {
    enum { N = 16 / CK + 2 };
    static _Thread_local uint8_t *kind; /* [y][cz][cx] over the chunk's cells plus a border */
    if (!kind) kind = xmalloc((size_t)GH * N * N);
    int x0 = MARGIN + cx * 16 - CK, z0 = MARGIN + cz * 16 - CK; /* grid block of cell (0,0) */
    for (int y = 0; y < GH; y++)
        for (int j = 0; j < N; j++)
            for (int i = 0; i < N; i++) {
                int n = 0, wat = 0;
                for (int dz = 0; dz < CK; dz++)
                    for (int dx = 0; dx < CK; dx++) {
                        int k = fine_kind(x0 + i * CK + dx, y, z0 + j * CK + dz);
                        n += k == 1; wat += k == 2;
                    }
                kind[((size_t)y * N + j) * N + i] = n * 4 >= CK * CK ? 1 : wat * 2 >= CK * CK ? 2 : 0;
            }
#define KIND(i, y, j) ((y) < 0 ? 1 : (y) >= GH ? 0 : kind[((size_t)(y) * N + (j)) * N + (i)])
    const float full_uv[4] = { 0, 0, 16, 16 };
    for (int y = 0; y < GH; y++)
        for (int j = 1; j < N - 1; j++)
            for (int i = 1; i < N - 1; i++) {
                int k = KIND(i, y, j);
                if (!k) continue;
                int bx = x0 + i * CK, bz = z0 + j * CK;
                float box[6] = { (float)bx, (float)y, (float)bz, (float)(bx + CK), (float)(y + 1), (float)(bz + CK) };
                if (k == 2) {
                    if (KIND(i, y + 1, j)) continue;
                    box[4] = y + 14 / 16.0f;
                    float q[4][3];
                    box_face(FACE_UP, box, q);
                    paint_t pt = { PAINT_BIOME, BC_WATER, { 1, 1, 1 } };
                    emit_quad(&w->mb[MAT_WATER], q, tex_water, full_uv, 0, &pt, 0, 0, 0, &w->quads);
                    continue;
                }
                for (int f = 0; f < FACE_COUNT; f++) {
                    if (KIND(i + DIRV[f][0], y + DIRV[f][1], j + DIRV[f][2]) == 1) continue;
                    double se[3] = { 0 }, sa[3] = { 0 };
                    int ne = 0, na = 0, nleaf = 0;
                    for (int dz = 0; dz < CK; dz++)
                        for (int dx = 0; dx < CK; dx++) {
                            int x = bx + dx, z = bz + dz;
                            if (!coarse_solid(x, y, z)) continue;
                            const state_t *st = &states[gget(x, y, z)];
                            float c[3] = { st->face_col[f][0], st->face_col[f][1], st->face_col[f][2] };
                            if (st->face_bc[f] >= 0) {
                                float p[3] = { x + 0.5f, y + 0.5f, z + 0.5f }, t[3];
                                biome_color_at(st->face_bc[f], p, t);
                                for (int a = 0; a < 3; a++) c[a] *= t[a];
                            }
                            nleaf += st->leaves || st->up_lit;
                            if (st->up_lit && st->shape && plant_far(x, y, z) == PLANT_SUNK) for (int a = 0; a < 3; a++) c[a] *= far_plant_dim(st);
                            int open = fine_kind(x + DIRV[f][0], y + DIRV[f][1], z + DIRV[f][2]) != 1;
                            for (int a = 0; a < 3; a++) { sa[a] += c[a]; if (open) se[a] += c[a]; }
                            na++; ne += open;
                        }
                    paint_t pt = { PAINT_FIXED, 0, { 0.2f, 0.2f, 0.2f } };
                    for (int a = 0; a < 3; a++)
                        if (ne) pt.fixed[a] = (float)(se[a] / ne);
                        else if (na) pt.fixed[a] = (float)(sa[a] / na);
                    float q[4][3];
                    box_face(f, box, q);
                    /* mostly leaves: lit as leaves, as the levels nearer by are */
                    emit_quad(&w->mb[MAT_OPAQUE], q, tex_white, full_uv, 0, &pt, 0, nleaf * 2 > na, 0, &w->quads);
                }
            }
#undef KIND
}
