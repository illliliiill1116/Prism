/*
 * prism bake: one region of a Minecraft Java world -> 3D map tiles with baked
 * lighting (sun visibility along its arc, sky, block and bounced light per
 * vertex), for the viewer in viewer/.
 *
 *   prism [bake] <client.jar> <world or region dir> <rx> <rz> <out dir> [threads] [glb]
 *
 * rx/rz may be fractional, rounded to a chunk: -0.5 -0.5 is a region-sized
 * window centred on the world origin, where four regions meet.
 * Writes <out>/atlas.png, <out>/index.json and <out>/tiles.bin, three
 * levels of detail per 64-block tile: level 0 in full, level 1 without
 * plants and with fast leaves, each as the quads they share and its own
 * (see pack_level_tiles), and level 2 (all tiles in one pack) in 4x1x4
 * cells painted with their blocks' mean colours, all gzipped in the
 * compact form tile_payload describes, in one file (see write_tiles). "glb" also writes the full level as
 * <out>/region.glb. Blocks MARGIN around the
 * region are loaded too, so faces at its edge cull and shadows from the
 * neighbours fall in. Lighting is linear RGB in COLOR_0, multiplied with
 * the texture by an unlit material (KHR_materials_unlit).
 *
 * COLOR_0 holds the light without the sun x tint at half scale; _SUN what
 * a full sun adds to it (a flat face square on, nothing in the way), its
 * alpha 0 marking plants and THIN_ALPHA a block that does not block the
 * bake's sky and block light rays (a fence, a post: see state_t occl;
 * the sun's it blocks far off, see far_occl), LEAF_ALPHA leaves (lit through: see LEAF_PASS);
 * _SUNWIN, up to two windows along the sun's arc
 * (see sun_window) in which it gets past far blockers (see NEAR0), as
 * angles 0..255 for 0..180 degrees: start, end, start, end. The viewer lights each corner for wherever the
 * sun is. _BLK is block light alone (torches, lanterns: rays to each,
 * so they cast shadows), same scale, of which COLOR_0 holds BLOCK_DAY;
 * the viewer dims the rest, the sky's part, at night and turns the block
 * light up to full. Water surfaces get their own material for the viewer's
 * reflection shader.
 *
 * Banners show their patterns and player heads their skins, read from the
 * block entities (skins from PRISM_SKINS, which scripts/fetch_skins.sh fills).
 */
#include "cli.h"

#define BAKE_BAND 1 /* tile rows meshed at once (see bake_region) */

/* A file written under a temporary name and renamed, so a reader never
 * sees half of it. */
int write_atomic(const char *path, const void *p, size_t n) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    int bad = !f || fwrite(p, 1, n, f) != n;
    if (f && fclose(f)) bad = 1;
    if (bad || rename(tmp, path)) { remove(tmp); return 1; }
    return 0;
}

static int write_atomic_png(const char *path, const canvas_t *img) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (png_encode_to_file(tmp, img) != PNG_OK || rename(tmp, path)) { remove(tmp); return 1; }
    return 0;
}

int cmd_bake(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: prism [bake] <client.jar> <world or region dir> <rx> <rz> <out dir> [threads] [glb]\n");
        return 2;
    }
    const char *jar = argv[1], *out = argv[5];
    char rdir[4096];
    region_dir(argv[2], rdir, sizeof rdir);
    /* the window's first chunk */
    int c0x = (int)lround(atof(argv[3]) * 32), c0z = (int)lround(atof(argv[4]) * 32);
    int nthreads = argc > 6 ? atoi(argv[6]) : 16;
    int want_glb = 0;
    for (int i = 7; i < argc; i++) {
        if (strcmp(argv[i], "glb") == 0) want_glb = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (nthreads < 1) { fprintf(stderr, "threads: 1 or more\n"); return 2; }
    big_allocs_mapped();
    if (prism_open(jar)) return 1;
    return bake_region(rdir, c0x, c0z, out, nthreads, want_glb);
}

/* The region's water surface: the commonest height (world y) of the top
 * block of its columns whose top is water, and how many columns have it;
 * 0 columns when none is. The viewer's ground mist lies on it. */
static int sea_level(long *cols) {
    static long hist[GH];
    memset(hist, 0, sizeof hist);
    for (int z = MARGIN; z < MARGIN + REG; z++)
        for (int x = MARGIN; x < MARGIN + REG; x++)
            for (int y = GH - 1; y >= 0; y--) {
                const state_t *s = &states[grid[gidx(x, y, z)]];
                if (s->air) continue;
                if (s->fluid == FLUID_WATER) hist[y]++;
                break;
            }
    int best = 0;
    for (int y = 1; y < GH; y++)
        if (hist[y] > hist[best]) best = y;
    *cols = hist[best];
    return best + YMIN;
}

/* The window from chunk c0x, c0z (a region and MARGIN around it) baked
 * into out, after prism_open. */
int bake_region(const char *rdir, int c0x, int c0z, const char *out, int nthreads, int want_glb) {
    double t0 = now_s();
    time_t baked = time(NULL);
    /* the light caches need hold only a band's corners (see below), and
     * are emptied after each */
    lc_bits = fc_bits = 21;
    canvas_t atlas;
    long lit, emitters;
    window_load(rdir, c0x, c0z, &atlas, &lit, &emitters);
    long sea_cols;
    int sea = sea_level(&sea_cols);
    /* CAVE_Y sits 7 under the usual sea (62); a map built around lower
     * water (an old superflat one at y 5) would lose every closed room of
     * its buildings to it, so it follows the water down, never up */
    cave_y = sea_cols && sea - 7 < CAVE_Y ? sea - 7 : CAVE_Y;
    double t_load = now_s();
    mkdir(out, 0755);
    /* the atlas URL carries its content hash, so a browser never pairs a
     * new mesh with an old atlas from its cache */
    uint32_t atlas_hash = 2166136261u;
    for (uint32_t y = 0; y < atlas.height; y++) {
        const uint8_t *row = canvas_row(&atlas, y);
        for (size_t i = 0; i < (size_t)atlas.width * 4; i++) atlas_hash = (atlas_hash ^ row[i]) * 16777619u;
    }
    char path[4096];
    static size_t tile_q0[2][MAT_COUNT][NTILES + 1], tile_quads[4][NTILES]; /* levels 0, 1, 2, shared part */
    static mbuf_t keep0[MAT_COUNT]; /* level 0, until level 1 is there to split it with */
    static bytes_t gzs[NTILES][3], l2; /* the tiles' parts, gzipped, until written */
    static uint64_t tile_at[NTILES][4];
    long quads[3] = { 0 }, mat_quads[MAT_COUNT] = { 0 };
    double t_lod[3] = { 0 };
    /* Levels 0 and 1 a band of tile rows at a time, each band's tiles split
     * and packed before the next is meshed: only a band's meshes are held,
     * not the region's (each tile's quads come from its own chunks in the
     * same order either way), and the light caches only a band's corners,
     * of both levels, which share most. For the glb, level 0 at once. Then
     * level 2, small, for the whole region. */
    const int TW = 32 / TILE_CHUNKS, rows = want_glb ? TW : BAKE_BAND;
    for (int b0 = 0; b0 < TW + 1; b0 += b0 < TW ? rows : 1) {
        int coarse = b0 >= TW;
        if (b0) light_caches_clear();
        for (int c = 0; c < 32 * 32; c++) {
            int row = c / 32 / TILE_CHUNKS;
            mesh_skip[c] = !coarse && (row < b0 || row >= b0 + rows);
        }
        for (int lod = coarse ? 2 : 0; lod < (coarse ? 3 : 2); lod++) {
            double t = now_s();
            lod_mode = lod;
            mbuf_t merged[MAT_COUNT];
            quads[lod] += mesh_pass(nthreads, merged, tile_q0[lod & 1]);
            if (lod == 0 && want_glb) {
                snprintf(path, sizeof path, "%s/region.glb", out);
                if (write_glb(path, merged, tile_q0[0], atlas_hash)) { fprintf(stderr, "can't write %s\n", path); return 1; }
            }
            size_t *lq[3] = { tile_quads[0], tile_quads[1], tile_quads[3] };
            if (lod == 1 && pack_level_tiles(keep0, tile_q0[0], merged, tile_q0[1], lq, gzs, b0 * TW, (b0 + rows) * TW, nthreads)) {
                fprintf(stderr, "can't pack level 0, 1 tiles\n");
                return 1;
            }
            if (lod == 2 && pack_level2(merged, tile_q0[0], tile_quads[2], &l2, nthreads)) { fprintf(stderr, "can't pack level 2 tiles\n"); return 1; }
            if (lod == 0) for (int m = 0; m < MAT_COUNT; m++) mat_quads[m] += (long)(merged[m].nv / 4);
            if (lod == 0) memcpy(keep0, merged, sizeof keep0);
            else mb_free(merged);
            if (lod == 1) mb_free(keep0);
            t_lod[lod] += now_s() - t;
        }
    }
    memset(mesh_skip, 0, sizeof mesh_skip);
    printf("level 0 quads: %ld (opaque %ld, cutout %ld, translucent %ld, water %ld)\n", quads[0],
           mat_quads[0], mat_quads[1], mat_quads[2], mat_quads[3]);
    free(lcache);
    lcache = NULL;
    free(fcache);
    fcache = NULL;
    size_t l2_len = l2.n;
    snprintf(path, sizeof path, "%s/" TILES_FILE, out);
    if (write_tiles(path, &l2, gzs, tile_at)) { fprintf(stderr, "can't write %s\n", path); return 1; }
    snprintf(path, sizeof path, "%s/atlas.png", out);
    if (write_atomic_png(path, &atlas)) { fprintf(stderr, "can't write %s\n", path); return 1; }

    /* index.json: the format, where the region sits in the world, where
     * level 2 is in tiles.bin and which tiles have anything: tx, tz, their
     * quads per level and in the shared part, then their offset in
     * tiles.bin and the lengths of their level 0, shared and level 1 parts.
     * "baked" is when it started (epoch seconds), for prism map to tell
     * which chunks changed since. "sea" is its water surface and the
     * columns that have it (sea_level), for map.json's. "ver" names this
     * bake, for the viewer to put on tile URLs: a browser's cached tile
     * then never outlives the bake it came from. */
    str_t idx = { 0 };
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint32_t ver = atlas_hash ^ fnv((const char *)&ts, sizeof ts);
    sappend(&idx, "{\"format\":%d,\"origin\":[%d,%d],\"atlas\":\"atlas.png?%08x\",\"ver\":\"%08x\",\"baked\":%lld,"
            "\"sea\":[%d,%ld],\"tile\":%d,\"l2\":[0,%zu],\"tiles\":[", MAP_FORMAT, reg_x0 + MARGIN + REG / 2,
            reg_z0 + MARGIN + REG / 2, atlas_hash, ver, (long long)baked, sea, sea_cols, TILE_CHUNKS * 16, l2_len);
    int first = 1;
    for (int t = 0; t < NTILES; t++) {
        if (!tile_quads[0][t] && !tile_quads[1][t] && !tile_quads[2][t]) continue;
        sappend(&idx, "%s[%d,%d,%zu,%zu,%zu,%zu,%llu,%llu,%llu,%llu]", first ? "" : ",", t % (32 / TILE_CHUNKS),
                t / (32 / TILE_CHUNKS), tile_quads[0][t], tile_quads[1][t], tile_quads[2][t], tile_quads[3][t],
                (unsigned long long)tile_at[t][0], (unsigned long long)tile_at[t][1],
                (unsigned long long)tile_at[t][2], (unsigned long long)tile_at[t][3]);
        first = 0;
    }
    sappend(&idx, "]}\n");
    snprintf(path, sizeof path, "%s/index.json", out);
    if (write_atomic(path, idx.p, idx.n)) { fprintf(stderr, "can't write %s\n", path); return 1; }
    free(idx.p);
    double t_write = now_s();

    printf("window at chunk %d,%d: %ld chunks loaded (incl. margin), %u block states, %u textures, atlas %ux%u\n",
           c0x, c0z, chunks_loaded, nstates, nslots, atlas_w, atlas_h);
    printf("biomes: %d\n", nbiomes);
    printf("block entities: %ld banners, %ld player heads (%ld skins missing), %ld pots with sherds, %u textures of their own, "
           "%ld moving blocks settled\n", be_banners, be_heads, be_missing_skins, be_pots, nbetex, be_moving);
    printf("sky light reached %ld voxels below their column top\n", lit);
    printf("block light: %ld emitting voxels\n", emitters);
    printf("quads per level: %ld, %ld, %ld\n", quads[0], quads[1], quads[2]);
    printf("time: load %.2f s, levels %.2f + %.2f + %.2f s (mesh, bake, write; %d threads), total %.2f s\n",
           t_load - t0, t_lod[0], t_lod[1], t_lod[2], nthreads, t_write - t0);
    return 0;
}
