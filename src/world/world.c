/* The world grid: a region plus MARGIN blocks around it as state indices,
 * biomes, the occlusion bitsets the rays walk, and sky and block light
 * flooded through it (propagate_sky, propagate_block). */
#include "prism.h"

uint16_t *grid;   /* state index, 0 = air (state 0 is interned as air) */

typedef struct { char name[96]; float col[BC_COUNT][3]; } biome_t;

static biome_t biomes[256];

int nbiomes;

uint8_t *bgrid;

tint_ctx_t *tints;

float lin_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f; }

float srgb_to_lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

void rgb8_lin(const uint8_t c[3], float out[3]) {
    for (int i = 0; i < 3; i++) out[i] = srgb_to_lin(c[i] / 255.0f);
}

/* effects.water_color from the jar's biome JSON; the game default otherwise. */
static void biome_water(const char *bare, float out[3]) {
    uint8_t c[3] = { 0x3F, 0x76, 0xE4 };
    char path[256];
    snprintf(path, sizeof path, "data/minecraft/worldgen/biome/%s.json", bare);
    const zip_entry_t *e = zip_findz(&jarzip, path);
    uint8_t *buf = NULL; size_t len = 0;
    if (e && zip_extract(&jarzip, e, &buf, &len) == ZIP_OK) {
        json_pool_t pool; json_pool_init(&pool);
        json_value_t *root;
        if (json_parse(&pool, (const char *)buf, len, &root) == JSON_OK) {
            json_value_t *fx = json_object_getz(&pool, root, "effects");
            json_value_t *wc = fx ? json_object_getz(&pool, fx, "water_color") : NULL;
            if (wc && wc->type == JSON_STRING && wc->string.len == 7 && wc->string.ptr[0] == '#') {
                unsigned v = (unsigned)strtoul(wc->string.ptr + 1, NULL, 16);
                c[0] = (uint8_t)(v >> 16); c[1] = (uint8_t)(v >> 8); c[2] = (uint8_t)v;
            } else if (wc && wc->type == JSON_NUMBER) {
                unsigned v = (unsigned)wc->number;
                c[0] = (uint8_t)(v >> 16); c[1] = (uint8_t)(v >> 8); c[2] = (uint8_t)v;
            }
        }
        json_pool_free(&pool);
        free(buf);
    }
    rgb8_lin(c, out);
}

static uint8_t biome_id(const char *name, uint16_t len) {
    for (int i = 0; i < nbiomes; i++)
        if (strlen(biomes[i].name) == len && memcmp(biomes[i].name, name, len) == 0) return (uint8_t)i;
    if (nbiomes == 256 || len >= sizeof biomes[0].name) return 0;
    biome_t *b = &biomes[nbiomes];
    memcpy(b->name, name, len); b->name[len] = 0;
    tint_prepare(tints, name, len);
    const tint_biome_t *tb = tint_biome_get(tints, name, len);
    rgb8_lin(tb->grass, b->col[BC_GRASS]);
    rgb8_lin(tb->foliage, b->col[BC_FOLIAGE]);
    rgb8_lin(tb->dry, b->col[BC_DRY]);
    biome_water(strncmp(b->name, "minecraft:", 10) == 0 ? b->name + 10 : b->name, b->col[BC_WATER]);
    return (uint8_t)nbiomes++;
}

static inline const biome_t *biome_cell(int cx, int cy, int cz) {
    cx = cx < 0 ? 0 : cx >= BW ? BW - 1 : cx;
    cz = cz < 0 ? 0 : cz >= BW ? BW - 1 : cz;
    cy = cy < 0 ? 0 : cy >= BH ? BH - 1 : cy;
    return &biomes[bgrid[((size_t)cy * BW + (size_t)cz) * BW + (size_t)cx]];
}

/* Biome colour `bc` at grid point p, bilinear between 4x4 cell centres. */
void biome_color_at(int bc, const float p[3], float out[3]) {
    float u = p[0] / 4 - 0.5f, v = p[2] / 4 - 0.5f;
    int cx = (int)floorf(u), cz = (int)floorf(v), cy = (int)floorf((p[1] - 0.01f) / 4);
    float fu = u - cx, fv = v - cz;
    const float *c00 = biome_cell(cx, cy, cz)->col[bc], *c10 = biome_cell(cx + 1, cy, cz)->col[bc];
    const float *c01 = biome_cell(cx, cy, cz + 1)->col[bc], *c11 = biome_cell(cx + 1, cy, cz + 1)->col[bc];
    for (int i = 0; i < 3; i++)
        out[i] = (c00[i] * (1 - fu) + c10[i] * fu) * (1 - fv) + (c01[i] * (1 - fu) + c11[i] * fu) * fv;
}

uint64_t *occl;   /* 1 bit per voxel: blocks light */

uint64_t *focc;   /* 1 bit per voxel: a thin blocker (far_occl) */

/* 1 bit per voxel: a plant (up_lit), which lets some of the sun through
 * (see ray_far_vis) */
uint64_t *pocc;

long chunks_loaded;

/* Sky light, 4 bits per voxel, computed like Minecraft's: 15 above each
 * column's top occluder, then spreading sideways and down through open
 * voxels, losing 1 per step. (26.2 saves SkyLight for few sections.)
 * Used only to drop caves, as BlueMap does. */
uint8_t *skyl;

int cave_y = CAVE_Y;

int16_t *coltop; /* highest occluder per column, -1 if none */

static inline void sky_set(size_t i, int v) {
    uint8_t *b = &skyl[i >> 1];
    *b = (uint8_t)((i & 1) ? ((*b & 0x0F) | (v << 4)) : ((*b & 0xF0) | v));
}

/* Block light, a byte per voxel: from each emitter, losing 1 per step
 * through anything but a full opaque block, as the game spreads it.
 * NULL when nothing in the grid gives off light. */
uint8_t *blkl;

emitter_t *emv;

uint32_t emc[EMIT_CW * EMIT_CW * EMIT_CH + 1];

int parse_chunk(const mca_region_t *r, int lx, int lz, nbt_pool_t *pool, nbt_tag_t **root) {
    mca_chunk_loc_t loc;
    mca_result_t e = mca_region_get_loc(r, lx, lz, &loc);
    if (e != MCA_OK) return e;
    if (!mca_chunk_present(&loc)) return MCA_ERR_EMPTY;
    return mca_region_parse_chunk(r, lx, lz, pool, root);
}

void load_chunk(nbt_pool_t *pool, nbt_tag_t *root, int gx0, int gz0) {
    nbt_tag_t *sections = nbt_compound_getz(pool, root, "sections");
    for (uint32_t s = 0; sections && s < sections->child_count; s++) {
        nbt_tag_t *sec = nbt_child_at(pool, sections, s);
        nbt_tag_t *yt = nbt_compound_getz(pool, sec, "Y");
        if (!yt) continue;
        int sy = yt->v_byte * 16 - YMIN;
        if (sy < 0 || sy >= GH) continue;
        uint16_t idx[CHUNK_BLOCKS_PER_SECTION];
        const nbt_tag_t *pal;
        if (chunk_decode_block_states(pool, sec, idx, &pal) != CHUNK_OK) continue;
        uint32_t map[4096];
        uint32_t npal = pal->child_count < 4096 ? pal->child_count : 4096;
        for (uint32_t p = 0; p < npal; p++) map[p] = intern_state(pool, pal, p);
        for (int i = 0; i < 4096; i++) {
            int x = i & 15, z = (i >> 4) & 15, y = i >> 8;
            int gx = gx0 + x, gz = gz0 + z;
            if (gx < 0 || gz < 0 || gx >= GW || gz >= GW) continue;
            grid[gidx(gx, sy + y, gz)] = (uint16_t)(idx[i] < npal ? map[idx[i]] : 0);
        }
        uint16_t bidx[CHUNK_BIOMES_PER_SECTION];
        const nbt_tag_t *bpal;
        if (chunk_decode_biomes(pool, sec, bidx, &bpal) == CHUNK_OK) {
            uint8_t bmap[64];
            uint32_t nb = bpal->child_count < 64 ? bpal->child_count : 64;
            for (uint32_t p = 0; p < nb; p++) {
                const nbt_tag_t *t = nbt_child_at(pool, bpal, p);
                bmap[p] = t->type == NBT_STRING ? biome_id(t->string.data, (uint16_t)t->string.len) : 0;
            }
            for (int i = 0; i < 64; i++) {
                int x = i & 3, z = (i >> 2) & 3, y = i >> 4;
                int bx = gx0 / 4 + x, bz = gz0 / 4 + z, by = sy / 4 + y;
                if (bx < 0 || bz < 0 || bx >= BW || bz >= BW || by < 0 || by >= BH) continue;
                bgrid[((size_t)by * BW + (size_t)bz) * BW + (size_t)bx] = bidx[i] < nb ? bmap[bidx[i]] : 0;
            }
        }
    }
    load_block_entities(pool, root);
    nbt_pool_free(pool);
    chunks_loaded++;
}

/* Doubles a full ring queue of capacity *cap (a power of two), its
 * entries [*h, *t) moved to the start. */
static uint32_t *grow_ring(uint32_t *q, size_t *cap, size_t *h, size_t *t) {
    size_t n = *t - *h, c = *cap * 2;
    uint32_t *nq = xmalloc(c * sizeof *nq);
    for (size_t k = 0; k < n; k++) nq[k] = q[(*h + k) & (*cap - 1)];
    free(q);
    *cap = c; *h = 0; *t = n;
    return nq;
}

/* Seeds: open voxels in direct sky light next to an open voxel that is
 * not (under an overhang, at a cave mouth). BFS from there. */
long propagate_sky(void) {
    size_t nvox = (size_t)GW * GW * GH;
    memset(skyl, 0, nvox / 2);
    if (!coltop) coltop = xmalloc((size_t)GW * GW * sizeof *coltop);
    for (int z = 0; z < GW; z++)
        for (int x = 0; x < GW; x++) {
            int t = -1;
            for (int y = occ_top; y >= 0; y--)
                if (occ_at(x, y, z)) { t = y; break; }
            coltop[z * GW + x] = (int16_t)t;
        }
    size_t qcap = 1 << 22, qh = 0, qt = 0;
    uint32_t *q = xmalloc(qcap * sizeof *q);
    /* a ring; full, it doubles (a world over the void lights everything
     * under it at once) */
#define PUSH(i) do { if (qt - qh == qcap) q = grow_ring(q, &qcap, &qh, &qt); q[qt++ & (qcap - 1)] = (uint32_t)(i); } while (0)
    static const int D[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, -1, 0 }, { 0, 1, 0 } };
    for (int z = 0; z < GW; z++)
        for (int x = 0; x < GW; x++) {
            int top = coltop[z * GW + x], hi = top;
            for (int d = 0; d < 4; d++) {
                int nx = x + D[d][0], nz = z + D[d][2];
                if (nx < 0 || nz < 0 || nx >= GW || nz >= GW) continue;
                if (coltop[nz * GW + nx] > hi) hi = coltop[nz * GW + nx];
            }
            for (int y = top + 1; y <= hi && y < GH; y++) PUSH(gidx(x, y, z));
        }
    long visited = 0;
    while (qh != qt) {
        uint32_t i = q[qh++ & (qcap - 1)];
        int x = (int)(i % GW), z = (int)((i / GW) % GW), y = (int)(i / ((size_t)GW * GW));
        int lv = sky_at(x, y, z);
        if (lv <= 1) continue;
        for (int d = 0; d < 6; d++) {
            int nx = x + D[d][0], ny = y + D[d][1], nz = z + D[d][2];
            if (nx < 0 || nz < 0 || nx >= GW || nz >= GW || ny < 0 || ny >= GH) continue;
            if (ny > coltop[nz * GW + nx] || occ_at(nx, ny, nz)) continue;
            size_t j = gidx(nx, ny, nz);
            if (((skyl[j >> 1] >> ((j & 1) * 4)) & 15) >= lv - 1) continue;
            sky_set(j, lv - 1);
            PUSH(j);
            visited++;
        }
    }
#undef PUSH
    free(q);
    return visited;
}

/* Brightest first, so each voxel is written once, with its final level. */
long propagate_block(void) {
    size_t nvox = (size_t)GW * GW * GH;
    struct { uint32_t *v; size_t n, c; } bk[16] = { 0 };
#define BPUSH(l, i) do { if (bk[l].n == bk[l].c) { bk[l].c = bk[l].c ? bk[l].c * 2 : 4096; \
        bk[l].v = realloc(bk[l].v, bk[l].c * sizeof *bk[l].v); if (!bk[l].v) { fprintf(stderr, "oom\n"); exit(1); } } \
        bk[l].v[bk[l].n++] = (uint32_t)(i); } while (0)
    long seeds = 0;
    /* a window loaded before this one left its own */
    if (blkl) memset(blkl, 0, nvox);
    free(emv);
    emv = NULL;
    memset(emc, 0, sizeof emc);
    for (size_t i = 0; i < nvox; i++) {
        int e = states[grid[i]].emit;
        if (!e) continue;
        if (!blkl) blkl = calloc(nvox, 1);
        if (!blkl) { fprintf(stderr, "oom\n"); exit(1); }
        blkl[i] = (uint8_t)e;
        BPUSH(e, i);
        seeds++;
    }
    static const int D[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, -1, 0 }, { 0, 1, 0 } };
    for (int lv = 15; lv > 1; lv--)
        for (size_t k = 0; k < bk[lv].n; k++) {
            uint32_t i = bk[lv].v[k];
            if (blkl[i] != lv) continue;
            int x = (int)(i % GW), z = (int)((i / GW) % GW), y = (int)(i / ((size_t)GW * GW));
            for (int d = 0; d < 6; d++) {
                int nx = x + D[d][0], ny = y + D[d][1], nz = z + D[d][2];
                if (nx < 0 || nz < 0 || nx >= GW || nz >= GW || ny < 0 || ny >= GH) continue;
                size_t j = gidx(nx, ny, nz);
                if (blkl[j] >= lv - 1 || states[grid[j]].cull) continue;
                blkl[j] = (uint8_t)(lv - 1);
                BPUSH(lv - 1, j);
            }
        }
#undef BPUSH
    for (int l = 0; l < 16; l++) free(bk[l].v);
    if (seeds) {
        emv = malloc((size_t)seeds * sizeof *emv);
        if (!emv) { fprintf(stderr, "oom\n"); exit(1); }
        memset(emc, 0, sizeof emc);
        for (int pass = 0; pass < 2; pass++) {
            uint32_t at[EMIT_CW * EMIT_CW * EMIT_CH];
            if (pass) {
                for (size_t c = 0, sum = 0; c <= EMIT_CW * EMIT_CW * EMIT_CH; c++) { size_t n = emc[c]; emc[c] = (uint32_t)sum; sum += n; }
                memcpy(at, emc, sizeof at);
            }
            for (size_t i = 0; i < nvox; i++) {
                int e = states[grid[i]].emit;
                if (!e) continue;
                int x = (int)(i % GW), z = (int)((i / GW) % GW), y = (int)(i / ((size_t)GW * GW));
                size_t c = ((size_t)(y / EMIT_CELL) * EMIT_CW + z / EMIT_CELL) * EMIT_CW + x / EMIT_CELL;
                if (!pass) emc[c]++;
                else emv[at[c]++] = (emitter_t){ (uint16_t)x, (uint16_t)y, (uint16_t)z, (uint8_t)e };
            }
        }
    }
    return seeds;
}
