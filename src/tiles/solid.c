/* solid.bin: what a walker in the viewer bumps into, per tile (see
 * collision_of for a block's). The viewer reads only the tiles around the
 * walker, with Range, so this stays out of tiles.bin and the walk is
 * optional: a map baked before it simply has none.
 *
 * "PSOL", u32 1, then per tile (NTILES, row by row) u32 offset, u32 length,
 * i16 lowest y, u16 rows; then the tiles, each zlib-deflated:
 * u16 palette length n, u32 coll x n, then a u8 palette index per block,
 * rows from the lowest y up, each z by z, x by x (TILE_CHUNKS * 16 square).
 * A tile with nothing to bump into has length 0. Little-endian. */
#include "prism.h"

#define TB (TILE_CHUNKS * 16)
#define SOLID_HEAD (8 + NTILES * 12)

typedef struct { int t0, t1; bytes_t *out; int *ylo, *rows, overflow; } solidjob_t;

static void put(uint8_t *p, uint32_t v, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static void solid_tile(int t, bytes_t *out, int *ylo, int *rows, int *overflow) {
    const int TW = 32 / TILE_CHUNKS, x0 = MARGIN + t % TW * TB, z0 = MARGIN + t / TW * TB;
    int lo = grid_h, hi = -1;
    for (int y = 0; y < grid_h; y++)
        for (int z = z0; z < z0 + TB && (lo > y || hi < y); z++)
            for (int x = x0; x < x0 + TB; x++)
                if (states[grid[gidx(x, y, z)]].coll) { if (y < lo) lo = y; hi = y; break; }
    *ylo = lo + grid_y0;
    *rows = hi < lo ? 0 : hi - lo + 1;
    out->n = 0;
    if (!*rows) return;
    size_t cells = (size_t)*rows * TB * TB;
    uint32_t pal[256];
    int npal = 1;
    pal[0] = 0;
    uint8_t *raw = xmalloc(2 + 4 * 256 + cells), *c = raw + 2 + 4 * 256;
    uint16_t memo_s = 0;
    uint8_t memo_i = 0;
    for (int y = lo; y <= hi; y++)
        for (int z = z0; z < z0 + TB; z++)
            for (int x = x0; x < x0 + TB; x++) {
                uint16_t s = grid[gidx(x, y, z)];
                if (s == memo_s) { *c++ = memo_i; continue; }
                uint32_t v = states[s].coll;
                int k = 0;
                while (k < npal && pal[k] != v) k++;
                if (k == npal) {
                    if (npal < 256) pal[npal++] = v;
                    else { k = 0; while (k < npal && pal[k] != 0xFFFFu) k++; if (k == npal) k = 0; ++*overflow; }
                }
                memo_s = s; memo_i = (uint8_t)k;
                *c++ = (uint8_t)k;
            }
    /* the palette right before the cells */
    uint8_t *h = raw + 4 * (256 - npal);
    put(h, (uint32_t)npal, 2);
    for (int k = 0; k < npal; k++) put(h + 2 + 4 * k, pal[k], 4);
    size_t n = (size_t)(c - h);
    uLongf cap = compressBound(n);
    out->p = realloc(out->p, cap);
    out->c = cap;
    if (compress2(out->p, &cap, h, n, 6) != Z_OK) cap = 0;
    out->n = cap;
    free(raw);
}

static void *solid_worker(void *arg) {
    solidjob_t *j = arg;
    for (int t = j->t0; t < j->t1; t++) solid_tile(t, &j->out[t], &j->ylo[t], &j->rows[t], &j->overflow);
    return NULL;
}

/* solid.bin's bytes for the region in grid, into file (the caller's to free) */
int pack_solid(bytes_t *file, int nthreads) {
    static bytes_t out[NTILES];
    int ylo[NTILES], rows[NTILES];
    if (nthreads > NTILES) nthreads = NTILES;
    pthread_t th[NTILES];
    solidjob_t jobs[NTILES];
    for (int i = 0; i < nthreads; i++) {
        jobs[i] = (solidjob_t){ NTILES * i / nthreads, NTILES * (i + 1) / nthreads, out, ylo, rows, 0 };
        pthread_create(&th[i], NULL, solid_worker, &jobs[i]);
    }
    int overflow = 0;
    for (int i = 0; i < nthreads; i++) { pthread_join(th[i], NULL); overflow += jobs[i].overflow; }
    if (overflow) fprintf(stderr, "solid.bin: %d blocks past a tile's 256 shapes, taken as whole blocks\n", overflow);
    size_t total = SOLID_HEAD;
    for (int t = 0; t < NTILES; t++) total += out[t].n;
    uint8_t *f = xmalloc(total), *p = f + SOLID_HEAD;
    memcpy(f, "PSOL", 4);
    put(f + 4, 1, 4);
    for (int t = 0; t < NTILES; t++) {
        uint8_t *e = f + 8 + 12 * t;
        put(e, out[t].n ? (uint32_t)(p - f) : 0, 4);
        put(e + 4, (uint32_t)out[t].n, 4);
        put(e + 8, (uint32_t)(uint16_t)(int16_t)ylo[t], 2);
        put(e + 10, (uint32_t)rows[t], 2);
        memcpy(p, out[t].p, out[t].n);
        p += out[t].n;
        free(out[t].p);
        out[t] = (bytes_t){ 0 };
    }
    *file = (bytes_t){ f, total, total };
    return 0;
}
