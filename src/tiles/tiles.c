/* Tiles: the compact columnar form the viewer decodes (tile_payload), level
 * 0 and 1 split into the part they share and their own, level 2 as a pack,
 * all of a region in one file (write_tiles). */
#include "prism.h"

/* One tile's quads, "M3T9" (a level part starts from the encoder's state
 * at the end of its shared part, see pack_level_tiles): u32 quads per material (x MAT_COUNT), u32 byte
 * length of each column (x TC_COUNT), then the columns, each padded to 4
 * bytes. The viewer unpacks them, losslessly, into the vertices the glb
 * holds (int16 x,y,z,0 in 1/POS_Q block from the region centre, uint16 u,v
 * normalized, COLOR_0 + _SUN + _SUNWIN + _BLK + _BNC + _BAMP): quads by material in emission order,
 * 4 vertices each. Per quad:
 *   mode  u8: bits 0-1 uv rotation k, 2 no sun at any corner (no window),
 *         3 plant (_SUN alpha 0; with hit bit 4, LEAF_ALPHA), 4-5 shape (QS_*), 6 uv as 4 corners,
 *         7 emits light: then emit u8, the colour alpha (else 0)
 *   p0    u16 x3: corner 0 minus the previous quad's, wrapping
 *   QS_AXIS: dir u8 d1 * 6 + d2 (d = axis * 2 + negative) and len u16 x2,
 *         edges along axes: corner 1 = 0 + e1, 2 = 0 + e1 + e2, 3 = 0 + e2
 *   QS_PARA: edge i16 x6, e1 then e2;  QS_ANY: edge i16 x9, corners 1-3 - 0
 *   uv    u16 x4: corner k's u,v then corner k+2's; corner i takes u from
 *         k if (i - k) & 3 is 0 or 3, else from k+2, and v from k if it is
 *         0 or 1. With bit 6, u16 x8, each corner's.
 *   hit   u8: bit i, corner i's colours are those of the last QS_AXIS
 *         corner at its position with the same dir (QS_AXIS quads only;
 *         other quads' bit 3: a plant hanging from above, see emit_quad);
 *         bit 4, _SUN alpha is THIN_ALPHA; bit 5, some corner has block
 *         light; bit 6, tinted; bit 7, colours kept as they are (raw)
 *   shade u8 unless raw, the light's falloff for the quad's normal x 255
 * Per corner not hit, the colours: raw, lit (rgb COLOR_0 then, unless
 * mode bit 2, rgb _SUN) and with hit bit 5 blk (rgb _BLK); else what bake()
 * made them of, which the viewer works out as light_from_par does (each
 * byte within 1 of the original, or the quad goes raw): sky u8, rays open
 * + 32 x wet samples; with bit 5 bq u8, block light x 255; with bit 6 tint
 * rgb; with wet samples watt u8 and wcol rgb, the water's light through and
 * colour. Then, unless mode bit 2:
 *   win   the first window's start and end in 4ths of the arc (255 as 64),
 *         the start + 128 when there is a second, in win2 the same way;
 *         each planar: every corner's first byte, then every second
 * and last bamp u8, _BAMP's first byte, and when it is not 0 bnc rgb, _BNC.
 * A plant's quad (mode bit 3, not a leaf) has pflag u8: bit 0, level 1
 * draws the plant too (plant_far), so the viewer does not fade it out.
 * Neighbouring faces share their corners' light, which is what hit saves;
 * a corner's light is a few numbers, which is what the rest saves (a third
 * of the bytes in all). */
enum { TC_MODE, TC_HIT, TC_DIR, TC_LEN, TC_EDGE, TC_P0, TC_UV, TC_LIT, TC_EMIT, TC_WIN, TC_BLK,
       TC_SHADE, TC_SKY, TC_BQ, TC_TINT, TC_WATT, TC_WCOL, TC_WIN2,
       TC_BAMP, TC_BNC, TC_PFLAG, TC_COUNT };

enum { QS_AXIS, QS_PARA, QS_ANY };

typedef struct { int16_t p[3]; uint8_t dir, used; uint8_t c[20]; } litslot_t;

/* What the encoder carries from quad to quad: the last colours at each
 * axis-quad corner (hit's table) and the last corner 0 (p0's delta). A
 * tile's level parts go on from its shared part's state (see tile_worker),
 * so while journal is set every change to seen is logged for rewind. */
typedef struct { size_t i; litslot_t old; } undo_t;

typedef struct {
    litslot_t *seen;
    size_t cap;
    int16_t prev[3], mark[3];
    undo_t *undo;
    size_t nundo, cundo;
    int journal;
} tenc_t;

static void tenc_init(tenc_t *e, size_t quads) {
    memset(e, 0, sizeof *e);
    e->cap = 16;
    while (e->cap < quads * 8) e->cap *= 2;
    e->seen = calloc(e->cap, sizeof *e->seen);
    if (!e->seen) { fprintf(stderr, "oom\n"); exit(1); }
}

static void tenc_mark(tenc_t *e) {
    memcpy(e->mark, e->prev, sizeof e->prev);
    e->nundo = 0;
    e->journal = 1;
}

static void tenc_rewind(tenc_t *e) {
    while (e->nundo) { e->nundo--; e->seen[e->undo[e->nundo].i] = e->undo[e->nundo].old; }
    memcpy(e->prev, e->mark, sizeof e->prev);
    e->journal = 0;
}

static void tenc_free(tenc_t *e) { free(e->seen); free(e->undo); }

/* The quads a payload holds: per material, indices into mb[m]. */
typedef struct { const mbuf_t *mb[MAT_COUNT]; const size_t *qi[MAT_COUNT]; size_t n[MAT_COUNT]; } quadset_t;

static int axis_dir(const int e[3], int *len) {
    int d = -1;
    for (int a = 0; a < 3; a++)
        if (e[a]) {
            if (d >= 0) return -1;
            d = a * 2 + (e[a] < 0);
            *len = abs(e[a]);
        }
    return d;
}

/* A corner's colour bytes (COLOR_0, _SUN, _BLK) from what bake() made
 * them of, as the viewer's lightFromPar works them out: par (see PSTRIDE),
 * with no sun when its windows are empty. All in doubles, as JavaScript
 * has them, in the same order, so both round alike. */
static void light_from_par(const uint8_t *R, int nosun, uint8_t out[9]) {
    const double F = SKY_FLOOR, MIN = LIGHT_MIN, MAX = LIGHT_MAX, BMAX = BLOCK_MAX, BD = BLOCK_DAY;
    double sky = F + (1 - F) * (R[0] & 31) / SKY_RAYS, blk = R[1] / 255.0, sh = R[5] / 255.0 * (double)ALBEDO;
    double wet = (R[0] >> 5) / 4.0, att = R[6] / 255.0;
    for (int c = 0; c < 3; c++) {
        double sc = SKY_COL[c], bc = BLOCK_COL[c], uc = SUN_COL[c];
        double base = sc * sky + BD * bc * blk, t = R[2 + c] / 255.0 * 0.5;
        double l1 = base < MIN ? MIN : base > MAX ? MAX : base;
        double l0 = base + uc > MAX ? MAX : base + uc;
        double lb = bc * blk > BMAX ? BMAX : bc * blk;
        l1 *= sh; l0 *= sh; lb *= sh;
        if (wet > 0) { /* bake()'s water: dimmed, taking on its colour */
            double wc = R[7 + c] / 255.0, f = 1 - wet + att * (0.75 + 0.25 * wc), g = (wet - att) * wc * 0.22;
            l1 = l1 * f + g; l0 = l0 * f + g; lb = lb * f + g;
        }
        double v[3] = { l1 * t, nosun ? 0 : (l0 - l1) * t, lb * t };
        for (int j = 0; j < 3; j++) out[j * 3 + c] = (uint8_t)(v[j] >= 1 ? 255 : v[j] <= 0 ? 0 : floor(v[j] * 255 + 0.5));
    }
}

static size_t tile_payload(bytes_t *out, const quadset_t *qs, tenc_t *e) {
    uint32_t nq[MAT_COUNT];
    size_t total = 0;
    for (int m = 0; m < MAT_COUNT; m++) total += nq[m] = (uint32_t)qs->n[m];
    if (!total) return 0;
    bytes_t col[TC_COUNT] = { 0 };
    const size_t cap = e->cap;
    litslot_t *seen = e->seen;
    int16_t *prev = e->prev;
    bytes_t win = { 0 }, win2 = { 0 };
    for (int m = 0; m < MAT_COUNT; m++) {
        const mbuf_t *mb = qs->mb[m];
        for (size_t k = 0; k < qs->n[m]; k++) {
            const size_t qi = qs->qi[m][k];
            int16_t P[4][3];
            uint16_t U[4][2];
            const uint8_t *C = mb->col + qi * 4 * VSTRIDE;
            for (int i = 0; i < 4; i++)
                for (int a = 0; a < 3; a++) P[i][a] = (int16_t)lrintf(mb->pos[(qi * 4 + i) * 3 + a] * POS_Q);
            for (int i = 0; i < 4; i++)
                for (int a = 0; a < 2; a++) {
                    float v = mb->uv[(qi * 4 + i) * 2 + a];
                    U[i][a] = (uint16_t)lrintf((v < 0 ? 0 : v > 1 ? 1 : v) * 65535.0f);
                }
            uint8_t mode = 0;

            int k;
            for (k = 0; k < 4; k++) {
                int ok = 1;
                for (int i = 0; i < 4 && ok; i++) {
                    int j = (i - k) & 3;
                    ok = U[i][0] == U[j == 0 || j == 3 ? k : (k + 2) & 3][0] && U[i][1] == U[j < 2 ? k : (k + 2) & 3][1];
                }
                if (ok) break;
            }
            if (k < 4) {
                mode |= (uint8_t)k;
                bput(&col[TC_UV], U[k], 4);
                bput(&col[TC_UV], U[(k + 2) & 3], 4);
            } else {
                mode |= 1 << 6;
                bput(&col[TC_UV], U, sizeof U);
            }

            uint16_t d0[3];
            for (int a = 0; a < 3; a++) { d0[a] = (uint16_t)(P[0][a] - prev[a]); prev[a] = P[0][a]; }
            bput(&col[TC_P0], d0, sizeof d0);
            int e1[3], e2[3], para = 1, l1 = 0, l2 = 0;
            for (int a = 0; a < 3; a++) {
                e1[a] = P[1][a] - P[0][a];
                e2[a] = P[3][a] - P[0][a];
                para &= P[2][a] == (int16_t)(P[1][a] + P[3][a] - P[0][a]);
            }
            int a1 = axis_dir(e1, &l1), a2 = axis_dir(e2, &l2), dir = -1;
            if (para && a1 >= 0 && a2 >= 0) {
                dir = a1 * 6 + a2;
                uint8_t d = (uint8_t)dir;
                uint16_t len[2] = { (uint16_t)l1, (uint16_t)l2 };
                bput(&col[TC_DIR], &d, 1);
                bput(&col[TC_LEN], len, sizeof len);
            } else if (para) {
                mode |= QS_PARA << 4;
                int16_t e[6] = { (int16_t)e1[0], (int16_t)e1[1], (int16_t)e1[2], (int16_t)e2[0], (int16_t)e2[1], (int16_t)e2[2] };
                bput(&col[TC_EDGE], e, sizeof e);
            } else {
                mode |= QS_ANY << 4;
                int16_t e[9];
                for (int i = 0; i < 9; i++) e[i] = (int16_t)(P[1 + i / 3][i % 3] - P[0][i % 3]);
                bput(&col[TC_EDGE], e, sizeof e);
            }

            int sun = 1;
            for (int i = 0; i < 4; i++) {
                const uint8_t *Ci = C + i * VSTRIDE;
                if (Ci[3] != C[3] || Ci[7] != C[7] || (C[7] != 0 && C[7] != THIN_ALPHA && C[7] != LEAF_ALPHA && C[7] != 255)) {
                    fprintf(stderr, "tile_payload: unexpected colour alpha\n");
                    exit(1);
                }
                sun &= Ci[8] > Ci[9] && Ci[10] > Ci[11];
            }
            if (sun) mode |= 1 << 2;
            if (!C[7] || C[7] == LEAF_ALPHA) mode |= 1 << 3;
            if (C[3]) { mode |= 1 << 7; bput(&col[TC_EMIT], C + 3, 1); }
            const uint8_t *R = mb->par + qi * 4 * PSTRIDE;
            int raw = 0, tinted = 0, blk = 0;
            for (int i = 0; i < 4 && !raw; i++) {
                const uint8_t *Ci = C + i * VSTRIDE, *Ri = R + i * PSTRIDE;
                uint8_t f[9];
                light_from_par(Ri, Ci[8] > Ci[9] && Ci[10] > Ci[11], f);
                const uint8_t want[9] = { Ci[0], Ci[1], Ci[2], Ci[4], Ci[5], Ci[6], Ci[12], Ci[13], Ci[14] };
                raw = Ri[0] == 255 || Ri[5] != R[5];
                for (int j = 0; j < 9; j++) raw |= abs(f[j] - want[j]) > 1;
                tinted |= Ri[2] != 255 || Ri[3] != 255 || Ri[4] != 255;
                blk |= Ri[1];
            }
            if (raw) {
                blk = 0;
                for (int i = 0; i < 4; i++) blk |= C[i * VSTRIDE + 12] | C[i * VSTRIDE + 13] | C[i * VSTRIDE + 14];
            } else
                bput(&col[TC_SHADE], R + 5, 1);
            uint8_t hit = (blk ? 1 << 5 : 0) | (raw ? 1 << 7 : tinted ? 1 << 6 : 0);
            for (int i = 0; i < 4; i++) {
                uint8_t c6[20];
                memcpy(c6, C + i * VSTRIDE, 3);
                memcpy(c6 + 3, C + i * VSTRIDE + 4, 3);
                memcpy(c6 + 6, C + i * VSTRIDE + 8, 4);
                memcpy(c6 + 10, C + i * VSTRIDE + 12, 3);
                memcpy(c6 + 13, C + i * VSTRIDE + 16, 3);
                memcpy(c6 + 16, C + i * VSTRIDE + 20, 4); /* all but the first 0 */
                litslot_t *s = NULL;
                if (dir >= 0) {
                    uint32_t h = ((uint32_t)(uint16_t)P[i][0] * 73856093u) ^ ((uint32_t)(uint16_t)P[i][1] * 19349663u) ^
                                 ((uint32_t)(uint16_t)P[i][2] * 83492791u) ^ ((uint32_t)dir * 2654435761u);
                    for (size_t j = h & (cap - 1);; j = (j + 1) & (cap - 1)) {
                        s = &seen[j];
                        if (!s->used || (s->dir == dir && !memcmp(s->p, P[i], sizeof s->p))) break;
                    }
                    if (s->used && !memcmp(s->c, c6, 20)) { hit |= (uint8_t)(1 << i); continue; }
                    if (e->journal) {
                        if (e->nundo == e->cundo) {
                            e->cundo = e->cundo ? e->cundo * 2 : 4096;
                            e->undo = realloc(e->undo, e->cundo * sizeof *e->undo);
                            if (!e->undo) { fprintf(stderr, "oom\n"); exit(1); }
                        }
                        e->undo[e->nundo++] = (undo_t){ (size_t)(s - seen), *s };
                    }
                    s->used = 1;
                    s->dir = (uint8_t)dir;
                    memcpy(s->p, P[i], sizeof s->p);
                    memcpy(s->c, c6, 20);
                }
                if (!sun) { /* in 4ths (255 as 64), a second window flagged by 128 on the first's start */
                    uint8_t w[4];
                    for (int j = 0; j < 4; j++) w[j] = c6[6 + j] == 255 ? 64 : c6[6 + j] >> 2;
                    int two = c6[8] <= c6[9];
                    int odd = !two && (c6[8] != 255 || c6[9]);
                    for (int j = 0; j < 4; j++) odd |= c6[6 + j] != 255 && (c6[6 + j] & 3);
                    if (odd) { fprintf(stderr, "tile_payload: sun window not as sun_window makes them\n"); exit(1); }
                    w[0] |= (uint8_t)(two << 7);
                    bput(&win, w, 2);
                    if (two) bput(&win2, w + 2, 2);
                }
                bput(&col[TC_BAMP], c6 + 16, 1);
                if (c6[16]) bput(&col[TC_BNC], c6 + 13, 3);
                if (raw) {
                    bput(&col[TC_LIT], c6, sun ? 3 : 6);
                    if (blk) bput(&col[TC_BLK], c6 + 10, 3);
                    continue;
                }
                const uint8_t *Ri = R + i * PSTRIDE;
                bput(&col[TC_SKY], Ri, 1);
                if (blk) bput(&col[TC_BQ], Ri + 1, 1);
                if (tinted) bput(&col[TC_TINT], Ri + 2, 3);
                if (Ri[0] >> 5) { bput(&col[TC_WATT], Ri + 6, 1); bput(&col[TC_WCOL], Ri + 7, 3); }
            }
            if (C[7] == THIN_ALPHA || C[7] == LEAF_ALPHA) hit |= 1 << 4;
            if (dir < 0 && C[15] & 1) hit |= 1 << 3;
            if (!C[7]) { uint8_t pf = C[15] >> 1 & 1; bput(&col[TC_PFLAG], &pf, 1); }
            bput(&col[TC_HIT], &hit, 1);
            bput(&col[TC_MODE], &mode, 1);
        }
    }
    for (int k = 0; k < 2; k++) {
        for (size_t i = k; i < win.n; i += 2) bput(&col[TC_WIN], win.p + i, 1);
        for (size_t i = k; i < win2.n; i += 2) bput(&col[TC_WIN2], win2.p + i, 1);
    }
    free(win.p);
    free(win2.p);
    bput(out, "M3T9", 4);
    bput(out, nq, sizeof nq);
    for (int c = 0; c < TC_COUNT; c++) { uint32_t n = (uint32_t)col[c].n; bput(out, &n, 4); }
    for (int c = 0; c < TC_COUNT; c++) {
        if (col[c].n) bput(out, col[c].p, col[c].n);
        bpad(out, 0);
        free(col[c].p);
    }
    return total;
}

/* data gzipped into out, a gzip member of its own (the viewer inflates
 * each part with DecompressionStream) */
static int gz(bytes_t *out, const uint8_t *data, size_t n) {
    z_stream zs = { 0 };
    *out = (bytes_t){ 0 };
    if (deflateInit2(&zs, 9, Z_DEFLATED, 15 + 16, 9, Z_DEFAULT_STRATEGY) != Z_OK) return 1;
    size_t cap = deflateBound(&zs, (uLong)n);
    uint8_t *buf = malloc(cap);
    if (!buf) { deflateEnd(&zs); return 1; }
    zs.next_in = (Bytef *)data;
    zs.avail_in = (uInt)n;
    zs.next_out = buf;
    zs.avail_out = (uInt)cap;
    int bad = deflate(&zs, Z_FINISH) != Z_STREAM_END;
    deflateEnd(&zs);
    if (bad) { free(buf); return 1; }
    *out = (bytes_t){ buf, zs.total_out, cap };
    return 0;
}

/* The quads of tile t, per material, as a quadset over merged. */
static void tile_quads_of(quadset_t *qs, size_t *ix[MAT_COUNT], const mbuf_t merged[MAT_COUNT],
                          size_t tile_q0[MAT_COUNT][NTILES + 1], int t) {
    for (int m = 0; m < MAT_COUNT; m++) {
        size_t a = tile_q0[m][t], n = tile_q0[m][t + 1] - a;
        ix[m] = malloc((n ? n : 1) * sizeof **ix);
        if (!ix[m]) { fprintf(stderr, "oom\n"); exit(1); }
        for (size_t i = 0; i < n; i++) ix[m][i] = a + i;
        qs->mb[m] = &merged[m]; qs->qi[m] = ix[m]; qs->n[m] = n;
    }
}

/* A quad's bytes, all of them: two quads with the same are the same quad. */
static int quad_same(const mbuf_t *a, size_t i, const mbuf_t *b, size_t j) {
    return !memcmp(a->pos + i * 12, b->pos + j * 12, 12 * sizeof(float)) &&
           !memcmp(a->uv + i * 8, b->uv + j * 8, 8 * sizeof(float)) &&
           !memcmp(a->col + i * 4 * VSTRIDE, b->col + j * 4 * VSTRIDE, 4 * VSTRIDE) &&
           !memcmp(a->par + i * 4 * PSTRIDE, b->par + j * 4 * PSTRIDE, 4 * PSTRIDE);
}

static uint64_t quad_hash(const mbuf_t *m, size_t i) {
    uint64_t h = 1469598103934665603u;
    const uint8_t *parts[4] = { (const uint8_t *)(m->pos + i * 12), (const uint8_t *)(m->uv + i * 8),
                                m->col + i * 4 * VSTRIDE, m->par + i * 4 * PSTRIDE };
    const size_t len[4] = { 12 * sizeof(float), 8 * sizeof(float), 4 * VSTRIDE, 4 * PSTRIDE };
    for (int k = 0; k < 4; k++)
        for (size_t j = 0; j < len[k]; j++) h = (h ^ parts[k][j]) * 1099511628211u;
    return h;
}

/* Tile t of levels 0 and 1 as three parts: shared, the quads both have
 * (in level 1's order), then level 0's others and level 1's others. */
static void split_tile(int t, const mbuf_t m0[MAT_COUNT], size_t q0[MAT_COUNT][NTILES + 1],
                       const mbuf_t m1[MAT_COUNT], size_t q1[MAT_COUNT][NTILES + 1],
                       quadset_t part[3], size_t *ix[3][MAT_COUNT]) {
    for (int m = 0; m < MAT_COUNT; m++) {
        size_t a0 = q0[m][t], n0 = q0[m][t + 1] - a0, a1 = q1[m][t], n1 = q1[m][t + 1] - a1;
        size_t cap = 16;
        while (cap < n0 * 2) cap *= 2;
        size_t *slot = malloc(cap * sizeof *slot); /* level 0 quad + 1, 0 empty */
        uint8_t *taken = calloc(n0 ? n0 : 1, 1);
        for (int k = 0; k < 3; k++) ix[k][m] = malloc(((k == 1 ? n0 : n1) + 1) * sizeof **ix);
        if (!slot || !taken || !ix[0][m] || !ix[1][m] || !ix[2][m]) { fprintf(stderr, "oom\n"); exit(1); }
        memset(slot, 0, cap * sizeof *slot);
        for (size_t i = 0; i < n0; i++) {
            size_t j = (size_t)quad_hash(&m0[m], a0 + i) & (cap - 1);
            while (slot[j]) j = (j + 1) & (cap - 1);
            slot[j] = i + 1;
        }
        size_t ns = 0, nb = 0, na = 0;
        for (size_t i = 0; i < n1; i++) {
            size_t j = (size_t)quad_hash(&m1[m], a1 + i) & (cap - 1), hit = 0;
            for (; slot[j]; j = (j + 1) & (cap - 1)) {
                size_t k = slot[j] - 1;
                if (!taken[k] && quad_same(&m0[m], a0 + k, &m1[m], a1 + i)) { taken[k] = 1; hit = 1; break; }
            }
            if (hit) ix[0][m][ns++] = a1 + i;
            else ix[2][m][na++] = a1 + i;
        }
        for (size_t i = 0; i < n0; i++) if (!taken[i]) ix[1][m][nb++] = a0 + i;
        free(slot); free(taken);
        part[0].mb[m] = &m1[m]; part[0].qi[m] = ix[0][m]; part[0].n[m] = ns;
        part[1].mb[m] = &m0[m]; part[1].qi[m] = ix[1][m]; part[1].n[m] = nb;
        part[2].mb[m] = &m1[m]; part[2].qi[m] = ix[2][m]; part[2].n[m] = na;
    }
}

typedef struct {
    bytes_t (*gz)[3]; /* levels 0, 1: per tile its parts, gzipped (shared, level 0's, level 1's) */
    mbuf_t *m0, *m1;
    size_t (*q0)[NTILES + 1], (*q1)[NTILES + 1];
    size_t *quads[3]; /* per tile: levels 0, 1 and their shared part */
    bytes_t *keep;    /* level 2: payloads kept for the pack (m1, q1 then hold it) */
    int t1;           /* the tiles next up to t1 */
    atomic_int next, bad;
} tilejob_t;

static size_t qs_total(const quadset_t *qs) {
    size_t n = 0;
    for (int m = 0; m < MAT_COUNT; m++) n += qs->n[m];
    return n;
}

static void *tile_worker(void *arg) {
    tilejob_t *j = arg;
    for (;;) {
        int t = atomic_fetch_add(&j->next, 1);
        if (t >= j->t1) return NULL;
        if (j->keep) {
            quadset_t qs;
            size_t *ix[MAT_COUNT];
            tile_quads_of(&qs, ix, j->m1, j->q1, t);
            tenc_t e;
            tenc_init(&e, qs_total(&qs));
            bytes_t b = { 0 };
            j->quads[2][t] = tile_payload(&b, &qs, &e);
            j->keep[t] = b;
            tenc_free(&e);
            for (int m = 0; m < MAT_COUNT; m++) free(ix[m]);
            continue;
        }
        quadset_t part[3];
        size_t *ix[3][MAT_COUNT];
        split_tile(t, j->m0, j->q0, j->m1, j->q1, part, ix);
        tenc_t e;
        tenc_init(&e, qs_total(&part[0]) + qs_total(&part[1]) + qs_total(&part[2]));
        /* shared, then level 0's part from where it left off, then rewound
         * to there, level 1's */
        size_t n[3];
        for (int k = 0; k < 3; k++) {
            if (k == 1) tenc_mark(&e);
            if (k == 2) tenc_rewind(&e);
            bytes_t b = { 0 };
            n[k] = tile_payload(&b, &part[k], &e);
            if (n[k]) j->gz[t][k] = b; /* gzipped after, a part a job (gz_worker) */
            else free(b.p);
        }
        tenc_free(&e);
        j->quads[2][t] = n[0];
        j->quads[0][t] = n[0] + n[1];
        j->quads[1][t] = n[0] + n[2];
        for (int k = 0; k < 3; k++)
            for (int m = 0; m < MAT_COUNT; m++) free(ix[k][m]);
    }
}

/* The parts tile_worker left, from tile next / 3 to t1, gzipped in place:
 * a job each, so a few tiles still keep every thread busy. */
static void *gz_worker(void *arg) {
    tilejob_t *j = arg;
    for (;;) {
        int i = atomic_fetch_add(&j->next, 1), t = i / 3;
        if (t >= j->t1) return NULL;
        bytes_t *b = &j->gz[t][i % 3], raw = *b;
        if (!raw.n) continue;
        if (gz(b, raw.p, raw.n)) atomic_store(&j->bad, 1);
        free(raw.p);
    }
}

static void run_tile_job(tilejob_t *job, int nthreads, void *(*fn)(void *)) {
    pthread_t th[64];
    if (nthreads > 64) nthreads = 64;
    for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, fn, job);
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
}

/* Levels 0 and 1 of tiles t0 to t1 as three parts each, gzipped into gz[t]: the quads
 * both levels have, then level 0's and level 1's others, which the encoder
 * writes on from where the shared part left off (so a level is its shared
 * part and then its own, decoded in that order). Fills quads[0], [1] per
 * tile with each level's quads and quads[2] with the shared part's. */
int pack_level_tiles(mbuf_t m0[MAT_COUNT], size_t q0[MAT_COUNT][NTILES + 1], mbuf_t m1[MAT_COUNT],
                     size_t q1[MAT_COUNT][NTILES + 1], size_t *quads[3], bytes_t gzs[NTILES][3], int t0, int t1,
                     int nthreads) {
    tilejob_t job = { .gz = gzs, .m0 = m0, .m1 = m1, .q0 = q0, .q1 = q1, .quads = { quads[0], quads[1], quads[2] }, .t1 = t1 };
    atomic_store(&job.next, t0);
    run_tile_job(&job, nthreads, tile_worker);
    atomic_store(&job.next, t0 * 3);
    run_tile_job(&job, nthreads, gz_worker);
    return atomic_load(&job.bad);
}

/* Level 2, small, as one pack: "M3PK", u32 count, count x { i32 tx,
 * i32 tz, u32 offset, u32 length }, then the tiles. Gzipped into l2. Fills
 * quads[t] per tile. */
int pack_level2(mbuf_t merged[MAT_COUNT], size_t tile_q0[MAT_COUNT][NTILES + 1], size_t quads[NTILES],
                bytes_t *l2, int nthreads) {
    const int TW = 32 / TILE_CHUNKS;
    static bytes_t keep[NTILES];
    tilejob_t job = { .m1 = merged, .q1 = tile_q0, .quads = { NULL, NULL, quads }, .keep = keep, .t1 = NTILES };
    run_tile_job(&job, nthreads, tile_worker);

    bytes_t body = { 0 }, pack = { 0 };
    uint32_t n = 0, ent[NTILES][4];
    for (int t = 0; t < NTILES; t++) {
        if (quads[t]) {
            ent[n][0] = (uint32_t)(t % TW); ent[n][1] = (uint32_t)(t / TW);
            ent[n][2] = (uint32_t)body.n; ent[n][3] = (uint32_t)keep[t].n;
            n++;
            bput(&body, keep[t].p, keep[t].n);
        }
        free(keep[t].p);
        keep[t] = (bytes_t){ 0 };
    }
    bput(&pack, "M3PK", 4);
    bput(&pack, &n, 4);
    for (uint32_t i = 0; i < n; i++) ent[i][2] += (uint32_t)(8 + 16 * n);
    bput(&pack, ent, 16 * n);
    if (body.n) bput(&pack, body.p, body.n);
    int bad = gz(l2, pack.p, pack.n);
    free(pack.p); free(body.p);
    return bad;
}

/* A region's tiles as one file (TILES_FILE): level 2's pack, then per tile
 * its level 0 part, shared part and level 1 part, so either level is one
 * run of bytes (the viewer fetches it with a Range request). at[t] gets
 * the tile's offset and its parts' lengths in that order. Frees the
 * parts. Written under a temporary name and renamed, so a reader never
 * sees half of it. */
int write_tiles(const char *path, bytes_t *l2, bytes_t gzs[NTILES][3], uint64_t at[NTILES][4]) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    int bad = !f || (l2->n && fwrite(l2->p, 1, l2->n, f) != l2->n);
    uint64_t off = l2->n;
    free(l2->p);
    *l2 = (bytes_t){ 0 };
    static const int order[3] = { 1, 0, 2 }; /* level 0's, shared, level 1's */
    for (int t = 0; t < NTILES; t++) {
        at[t][0] = off;
        for (int k = 0; k < 3; k++) {
            bytes_t *b = &gzs[t][order[k]];
            bad = bad || (b->n && fwrite(b->p, 1, b->n, f) != b->n);
            at[t][1 + k] = b->n;
            off += b->n;
            free(b->p);
            *b = (bytes_t){ 0 };
        }
    }
    if (f && fclose(f)) bad = 1;
    return bad || rename(tmp, path);
}
