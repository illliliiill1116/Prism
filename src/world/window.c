/*
 * The world around a window: prism_open once (the jar's models, the
 * tables every window shares), then window_load per window, a REG x REG
 * area of chunks with MARGIN blocks around it, which replaces the last
 * one: blocks, biomes, block entities, the light flood fills, the atlas.
 */
#include "prism.h"
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "out of memory (%zu bytes)\n", n); exit(1); }
    return p;
}

int prism_open(const char *jar) {
    if (blockmodel_open(jar, &reg) != BLOCKMODEL_OK) { fprintf(stderr, "can't open %s\n", jar); return -1; }
    if (zip_open(jar, &jarzip) != ZIP_OK) { fprintf(stderr, "can't open %s\n", jar); return -1; }
    tints = tint_create(jar, reg);
    bgrid = calloc((size_t)BW * BW * BH, 1);
    if (!tints || !bgrid || !tint_set_shards(tints, 1)) { fprintf(stderr, "oom\n"); return -1; }
    slot_hash = calloc(slot_hcap, sizeof *slot_hash);
    htab = calloc(hcap, sizeof *htab);
    cap_states = 1024;
    states = xmalloc(cap_states * sizeof *states);
    tex_water = blockmodel_texture(reg, "minecraft:block/water_still", 27);
    tex_lava = blockmodel_texture(reg, "minecraft:block/lava_still", 26);
    if (!tex_water || !tex_lava) { fprintf(stderr, "no fluid textures\n"); return -1; }
    tex_slot(tex_water); tex_slot(tex_lava);
    static canvas_t white;
    white = canvas_create(16, 16);
    memset(white.pixels, 0xFF, (size_t)16 * 16 * 4);
    tex_white = &white;
    tex_slot(tex_white);

    skin_dir = getenv("PRISM_SKINS");
    bepos = calloc(bepos_cap, sizeof *bepos);
    grid = calloc((size_t)GW * GW * GH, sizeof *grid);
    skyl = malloc((size_t)GW * GW * GH / 2);
    if (!grid || !skyl) { fprintf(stderr, "oom\n"); return -1; }
    memset(skyl, 0xFF, (size_t)GW * GW * GH / 2);
    /* state 0 = air, so a zeroed grid is empty */
    {
        state_t *s = &states[nstates];
        memset(s, 0, sizeof *s);
        s->key = strdup("minecraft:air");
        s->air = 1;
        htab[fnv(s->key, strlen(s->key)) & (hcap - 1)] = ++nstates;
    }
    size_t nvox = (size_t)GW * GW * GH;
    occl = calloc((nvox + 63) / 64, 8);
    focc = calloc((nvox + 63) / 64, 8);
    pocc = calloc((nvox + 63) / 64, 8);
    if (!occl || !focc || !pocc) { fprintf(stderr, "oom\n"); return -1; }
    setup_dirs();
    setup_bounce();
    return 0;
}

/* A region's chunks in the window, parsed on worker threads and put in
 * the grid on this one as each is ready, in order (states are interned
 * in the order met, so the same as one thread). */
typedef struct {
    const mca_region_t *r;
    int n;
    int16_t at[1024][2]; /* lx, lz */
    nbt_pool_t pool[1024];
    nbt_tag_t *root[1024];
    _Atomic int state[1024]; /* 0 waiting, else 1 + parse_chunk's result */
    _Atomic int next;
} rjob_t;

static void *parse_worker(void *arg) {
    rjob_t *J = arg;
    for (;;) {
        int k = atomic_fetch_add_explicit(&J->next, 1, memory_order_relaxed);
        if (k >= J->n) break;
        int e = parse_chunk(J->r, J->at[k][0], J->at[k][1], &J->pool[k], &J->root[k]);
        atomic_store_explicit(&J->state[k], 1 + e, memory_order_release);
    }
    return NULL;
}

/* What went wrong reading a region file or a chunk, for the bake's log */
static const char *mca_why(int e) {
    switch (e) {
    case MCA_ERR_IO: return "can't open it";
    case MCA_ERR_TOO_SMALL: return "shorter than its 8 KiB header";
    case MCA_ERR_BOUNDS: return "header points outside the file";
    case MCA_ERR_EXTERNAL: return "kept in a .mcc file (not read)";
    case MCA_ERR_BAD_COMPRESSION: return "compressed other than zlib/gzip (LZ4 is not read)";
    case MCA_ERR_INFLATE: return "compressed data broken";
    case MCA_ERR_NBT: return "NBT broken";
    default: return "unknown error";
    }
}

/* Loads a region file's chunks within cmin..cmax; those that could not be
 * read count in bad[] by mca_result_t when inside the window's own c0x,
 * c0z (32 x 32 chunks: the margin's are some other bake's to report). */
static void load_region(const mca_region_t *r, int rrx, int rrz, int cmin_x, int cmin_z, int cmax_x, int cmax_z,
                        int c0x, int c0z, int bad[MCA_ERR_NBT + 1]) {
    static rjob_t J;
    J.r = r;
    J.n = 0;
    for (int lz = 0; lz < 32; lz++)
        for (int lx = 0; lx < 32; lx++) {
            int cx = rrx * 32 + lx, cz = rrz * 32 + lz;
            if (cx < cmin_x || cx > cmax_x || cz < cmin_z || cz > cmax_z) continue;
            J.at[J.n][0] = (int16_t)lx;
            J.at[J.n][1] = (int16_t)lz;
            atomic_store_explicit(&J.state[J.n], 0, memory_order_relaxed);
            J.n++;
        }
    atomic_store(&J.next, 0);
    long nt = sysconf(_SC_NPROCESSORS_ONLN);
    nt = nt < 1 ? 1 : nt > 64 ? 64 : nt;
    if (nt > J.n) nt = J.n;
    pthread_t th[64];
    for (long i = 0; i < nt; i++) pthread_create(&th[i], NULL, parse_worker, &J);
    for (int k = 0; k < J.n; k++) {
        int st;
        while (!(st = atomic_load_explicit(&J.state[k], memory_order_acquire))) sched_yield();
        if (st != 1 + MCA_OK) {
            int cx = rrx * 32 + J.at[k][0], cz = rrz * 32 + J.at[k][1];
            if (st != 1 + MCA_ERR_EMPTY && cx >= c0x && cx < c0x + 32 && cz >= c0z && cz < c0z + 32 && st - 1 <= MCA_ERR_NBT)
                bad[st - 1]++;
            continue;
        }
        load_chunk(&J.pool[k], J.root[k], (rrx * 32 + J.at[k][0] - cmin_x) * 16, (rrz * 32 + J.at[k][1] - cmin_z) * 16);
    }
    for (long i = 0; i < nt; i++) pthread_join(th[i], NULL);
}

/* Loads the window whose first chunk is c0x, c0z (and MARGIN around it),
 * and lights it; *atlas is the atlas of every texture so far. */
void window_load(const char *rdir, int c0x, int c0z, canvas_t *atlas, long *lit, long *emitters) {
    reg_x0 = c0x * 16 - MARGIN;
    reg_z0 = c0z * 16 - MARGIN;
    memset(grid, 0, (size_t)GW * GW * GH * sizeof *grid);
    memset(skyl, 0xFF, (size_t)GW * GW * GH / 2);
    memset(bgrid, 0, (size_t)BW * BW * BH);
    memset(bepos, 0, bepos_cap * sizeof *bepos);
    nbepos = 0;
    memset(mesh_skip, 0, sizeof mesh_skip);
    mesh_y0 = 0;
    chunks_loaded = be_banners = be_heads = be_missing_skins = be_pots = be_moving = 0;
    /* load the region plus MARGIN blocks around it */
    int cmin_x = c0x - MARGIN / 16, cmax_x = c0x + 32 + MARGIN / 16 - 1;
    int cmin_z = c0z - MARGIN / 16, cmax_z = c0z + 32 + MARGIN / 16 - 1;
    for (int rrz = cmin_z >> 5; rrz <= cmax_z >> 5; rrz++)
        for (int rrx = cmin_x >> 5; rrx <= cmax_x >> 5; rrx++) {
            char path[4096];
            snprintf(path, sizeof path, "%s/r.%d.%d.mca", rdir, rrx, rrz);
            mca_region_t r;
            int bad[MCA_ERR_NBT + 1] = { 0 }, own = rrx >= c0x >> 5 && rrx <= (c0x + 31) >> 5 && rrz >= c0z >> 5 && rrz <= (c0z + 31) >> 5;
            mca_result_t e = mca_region_open(path, &r);
            struct stat st;
            if (e != MCA_OK) {
                if (own && stat(path, &st) == 0) printf("warning: r.%d.%d.mca: unreadable (%s), left empty\n", rrx, rrz, mca_why(e));
                mca_region_close(&r);
                continue;
            }
            load_region(&r, rrx, rrz, cmin_x, cmin_z, cmax_x, cmax_z, c0x, c0z, bad);
            mca_region_close(&r);
            for (int k = 0; k <= MCA_ERR_NBT; k++)
                if (bad[k]) printf("warning: r.%d.%d.mca: %d chunks unreadable (%s), left empty\n", rrx, rrz, bad[k], mca_why(k));
        }
    size_t nvox = (size_t)GW * GW * GH;
    memset(occl, 0, (nvox + 63) / 64 * 8);
    memset(focc, 0, (nvox + 63) / 64 * 8);
    memset(pocc, 0, (nvox + 63) / 64 * 8);
    occ_top = 0;
    for (int i = 0; i < HM_W * HM_W; i++) hm_top[i] = -1;
    for (int i = 0; i < HM_BW * HM_BW; i++) hm_big[i] = -1;
    for (size_t i = 0; i < nvox; i++)
        if (states[grid[i]].occl || states[grid[i]].far_occl || states[grid[i]].up_lit) {
            /* the heights (hm_skip, occ_top) take thin blockers and plants too */
            if (states[grid[i]].occl) occl[i >> 6] |= 1ull << (i & 63);
            else if (states[grid[i]].far_occl) focc[i >> 6] |= 1ull << (i & 63);
            else pocc[i >> 6] |= 1ull << (i & 63);
            int y = (int)(i / ((size_t)GW * GW)), x = (int)(i % GW), z = (int)((i / GW) % GW);
            if (y > occ_top) occ_top = y;
            int16_t *h = &hm_top[(z / HM_CELL) * HM_W + x / HM_CELL];
            if (y > *h) *h = (int16_t)y;
            h = &hm_big[(z / HM_BIG) * HM_BW + x / HM_BIG];
            if (y > *h) *h = (int16_t)y;
        }
    /* plants on the ground (grass, flowers, the two halves of a tall one)
     * cast no baked shade: a meadow would shade itself at a low sun; nor
     * do ones under water (kelp), which the water dims already */
    for (size_t i = 0; i < nvox; i++)
        if (pocc[i >> 6] >> (i & 63) & 1) {
            int y = (int)(i / ((size_t)GW * GW)), x = (int)(i % GW), z = (int)((i / GW) % GW);
            if (states[grid[i]].fluid || (y >= 1 && occ_at(x, y - 1, z)) ||
                (y >= 2 && states[grid[gidx(x, y - 1, z)]].up_lit && occ_at(x, y - 2, z)))
                pocc[i >> 6] &= ~(1ull << (i & 63));
        }
    for (uint32_t i = 0; i < nbetex; i++) tex_slot(&betex[i].tex);
    *lit = propagate_sky();
    *emitters = propagate_block();
    *atlas = build_atlas();
    prepare_face_colors();
    /* the light caches are by grid position: the last window's are wrong here */
    light_caches_clear();
}

void light_caches_clear(void) {
    if (lcache) memset(lcache, 0, ((size_t)1 << lc_bits) * sizeof *lcache);
    else lcache = calloc((size_t)1 << lc_bits, sizeof *lcache);
    if (fcache) memset(fcache, 0, ((size_t)1 << fc_bits) * sizeof *fcache);
    else fcache = calloc((size_t)1 << fc_bits, sizeof *fcache);
    if (!lcache || !fcache) { fprintf(stderr, "oom\n"); exit(1); }
}

/* Empties every column outside world x0..x1, z0..z1 (inclusive), and
 * inside them what is below ymin, after the light is worked out, so the
 * mesh ends there: the sides against those columns are drawn (the ground
 * cut open), while the light, shadows included, is still the whole
 * world's. coltop -1 marks them (see isodraw's cut walls). */
void window_clear_outside(int x0, int z0, int x1, int z1, int ymin) {
    int below = ymin - YMIN < 0 ? 0 : ymin - YMIN > GH ? GH : ymin - YMIN;
    /* the area's x span in grid cells, clamped; in the grid's own order */
    int ax0 = x0 - reg_x0, ax1 = x1 - reg_x0, az0 = z0 - reg_z0, az1 = z1 - reg_z0;
    ax0 = ax0 < 0 ? 0 : ax0; ax1 = ax1 >= GW ? GW - 1 : ax1;
    for (int y = 0; y < GH; y++)
        for (int z = 0; z < GW; z++) {
            uint16_t *row = &grid[gidx(0, y, z)];
            if (y < below || z < az0 || z > az1 || ax0 > ax1) { memset(row, 0, GW * sizeof *row); continue; }
            memset(row, 0, (size_t)ax0 * sizeof *row);
            memset(row + ax1 + 1, 0, (size_t)(GW - 1 - ax1) * sizeof *row);
        }
    for (int z = 0; z < GW; z++)
        for (int x = 0; x < GW; x++)
            if (z < az0 || z > az1 || x < ax0 || x > ax1) coltop[z * GW + x] = -1;
    /* nothing left there to mesh */
    mesh_y0 = below;
    for (int c = 0; c < 32 * 32; c++) {
        int gx = MARGIN + c % 32 * 16, gz = MARGIN + c / 32 * 16;
        mesh_skip[c] = gx + 15 < ax0 || gx > ax1 || gz + 15 < az0 || gz > az1;
    }
}
