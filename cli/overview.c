/*
 * prism overview: regions as small isometric pictures (iso.c's drawing, in
 * its projection), quick, for picking an area to bake (the app's picker).
 *
 *   prism overview <client.jar> <world or region dir> <out dir> <rx> <rz> [<rx> <rz>...]
 *                  [--scale px] [--ymin y] [--threads n]
 *
 * Each region is drawn a quarter at a time, each loaded as a window with
 * the world around it, as iso draws an area: cut at ymin but not at the
 * region's edges, so the pictures of neighbours meet without a seam (no cut
 * walls, and the water at the edges sees the ground under it). The cut is
 * at y 0, or lower where the ground is (a world built below 0): 8 under a
 * quarter's ground, the lowest tenth of its columns' tops. --ymin sets it
 * instead. With the sun but no cast shadows and the least bounced light.
 * It writes <out>/r.<x>.<z>.png, r.<x>.<z>.h.png, the world y each pixel
 * shows (iso_image_write_heights), and r.<x>.<z>.json: {"scale", "ymin"
 * (the lowest cut), "ground" (the lowest quarter's, 63 if none), "x", "y",
 * "w", "h"}, the pictures' corner in projected pixels (iso_project) and
 * their size, w 0 and no pictures when the region has nothing in it.
 * Pictures of neighbours drawn in that order by rx + rz, each at its
 * corner, make the world.
 */
#include "cli.h"
#include <limits.h>

#define Q 16   /* a quarter's side, in chunks: drawn at once */
#define BACK 4 /* and meshed behind it: what its water looks down to (iso.c's BACK) */

static int write_text(const char *path, const char *s) { return write_atomic(path, s, strlen(s)); }

/* The ground of the window's quarter at qx, qz (chunks), loaded: the
 * lowest tenth of its columns' tops, world y; INT_MAX if it has none */
static int ground_of(int qx, int qz) {
    static int count[GRID_H_MAX + 1];
    memset(count, 0, sizeof count);
    int n = 0;
    for (int z = 0; z < Q * 16; z++)
        for (int x = 0; x < Q * 16; x++) {
            int gx = qx * 16 + x - reg_x0, gz = qz * 16 + z - reg_z0;
            int t = coltop[gz * GW + gx];
            if (t >= 0) { count[t]++; n++; }
        }
    if (!n) return INT_MAX;
    int y = 0;
    for (int seen = 0; y < grid_h; y++)
        if ((seen += count[y]) * 10 >= n) break;
    return y + grid_y0;
}

int cmd_overview(int argc, char **argv) {
    const char *usage = "usage: prism overview <client.jar> <world or region dir> <out dir> <rx> <rz> [<rx> <rz>...]\n"
                        "                      [--scale px] [--ymin y] [--threads n]\n";
    if (argc < 6) { fputs(usage, stderr); return 2; }
    const char *jar = argv[1], *out = argv[3];
    char rdir[4096];
    region_dir(argv[2], rdir, sizeof rdir);
    iso_opts_t o = { .scale = 2, .ss = 1, .shadows = 0, .threads = plat_ncpu(), .sun = 136.4f };
    int ymin = 0, fixed = 0, n = 0;
    static int regs[4096][2];
    for (int i = 4; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (a[0] == '-' && a[1] == '-') {
            if (!v) { fputs(usage, stderr); return 2; }
            if (!strcmp(a, "--scale")) o.scale = atoi(v);
            else if (!strcmp(a, "--ymin")) ymin = atoi(v), fixed = 1;
            else if (!strcmp(a, "--threads")) o.threads = atoi(v);
            else { fprintf(stderr, "unknown option %s\n%s", a, usage); return 2; }
            i++;
            continue;
        }
        if (!v || n == 4096) { fputs(usage, stderr); return 2; }
        regs[n][0] = atoi(a);
        regs[n][1] = atoi(v);
        n++;
        i++;
    }
    if (!n || o.scale < 1 || o.scale > 16 || o.threads < 1 || ymin < WORLD_Y_MIN || ymin > WORLD_Y_MAX) {
        fprintf(stderr, "--scale is 1..16, --ymin -2032..4063, --threads 1 or more, and a region at least\n");
        return 2;
    }
    big_allocs_mapped();
    if (prism_open(jar)) return 1;
    plat_mkdir(out);
    light_one_sun(o.sun * 3.14159265f / 180);
    light_defer = 1;
    lc_bits = fc_bits = 21; /* a quarter's corners */
    set_bounce_rays(1);
    int failed = 0;
    for (int k = 0; k < n; k++) {
        double t0 = now_s();
        int rx = regs[k][0], rz = regs[k][1];
        int x0 = rx * REG, z0 = rz * REG, x1 = x0 + REG - 1, z1 = z0 + REG - 1;
        char path[4096], json[256];
        long box[4] = { 0, 0, 0, 0 }, quads = 0;
        int enc[2] = { -64, 128 };
        iso_image_t img;
        int made = !iso_image_init(&img, o.scale, 0, x0, z0, x1, z1, fixed ? ymin : grid_y0), bad = !made, low = ymin, ground = INT_MAX;
        if (made) iso_image_heights(&img);
        /* a quarter of the region at a time, back (-x, -z) to front, each
         * loaded with BACK chunks behind it meshed too: what its water looks
         * down to past the quarter's back edges. Level 0: level 1's far
         * plants and leaves show up wrong this close (hanging vines as
         * blades) */
        static const int quarter[4][2] = { { 0, 0 }, { Q, 0 }, { 0, Q }, { Q, Q } };
        for (int w = 0; w < 4 && !bad; w++) {
            int qx = rx * 32 + quarter[w][0], qz = rz * 32 + quarter[w][1];
            canvas_t atlas;
            long lit, emitters;
            /* only the chunks it meshes and a margin around them: the rest
             * of the window would only take time to read and light */
            window_part_on = 1;
            window_part[0] = qx - BACK - MARGIN / 16;
            window_part[1] = qz - BACK - MARGIN / 16;
            window_part[2] = qx + Q - 1 + MARGIN / 16;
            window_part[3] = qz + Q - 1 + MARGIN / 16;
            window_load(rdir, qx - BACK, qz - BACK, &atlas, &lit, &emitters);
            /* only below ymin: the world around stays, so the edges aren't cut */
            int g = chunks_loaded ? ground_of(qx, qz) : INT_MAX, cut = ymin;
            if (g < ground) ground = g;
            if (!fixed && g != INT_MAX && g - 8 < cut) cut = g - 8 < grid_y0 ? grid_y0 : g - 8;
            if (cut < low) low = cut;
            window_clear_outside(x0 - REG, z0 - REG, x1 + REG, z1 + REG, cut);
            for (int c = 0; c < 32 * 32; c++) mesh_skip[c] = c % 32 >= BACK + Q || c / 32 >= BACK + Q;
            lod_mode = 0;
            if (chunks_loaded) {
                static size_t q0[MAT_COUNT][NTILES + 1];
                mbuf_t merged[MAT_COUNT];
                long q = mesh_pass(o.threads, merged, q0);
                int core[4] = { qx * 16, qz * 16, (qx + Q) * 16 - 1, (qz + Q) * 16 - 1 };
                if (q && iso_draw(&img, merged, &atlas, &o, core)) bad = 1;
                mb_free(merged);
                light_caches_clear();
                quads += q;
            }
            canvas_free(&atlas);
        }
        memset(mesh_skip, 0, sizeof mesh_skip);
        window_part_on = 0;
        snprintf(path, sizeof path, "%s/r.%d.%d.png", out, rx, rz);
        if (!bad && quads) {
            if (iso_image_write(&img, path, o.threads, box)) bad = 1;
            else {
                snprintf(path, sizeof path, "%s/r.%d.%d.h.png", out, rx, rz);
                if (iso_image_write_heights(&img, path, o.threads, box, enc)) bad = 1;
            }
        }
        if (bad) {
            fprintf(stderr, "r.%d.%d: can't draw it\n", rx, rz);
            failed++;
            box[2] = 0;
        }
        if (made) iso_image_free(&img);
        /* hy, hk: the heights picture's encoding, (y - hy) * hk + 1, when
         * not the usual -64, 128 (iso_image_write_heights) */
        char hk[64] = "";
        if (enc[0] != -64 || enc[1] != 128) snprintf(hk, sizeof hk, ",\"hy\":%d,\"hk\":%d", enc[0], enc[1]);
        snprintf(json, sizeof json, "{\"scale\":%d,\"ymin\":%d,\"ground\":%d,\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld%s}\n", o.scale,
                 low, ground == INT_MAX ? 63 : ground, box[0], box[1], box[2], box[3], hk);
        snprintf(path, sizeof path, "%s/r.%d.%d.json", out, rx, rz);
        if (write_text(path, json)) { fprintf(stderr, "can't write %s\n", path); return 1; }
        printf("r.%d.%d: %ld x %ld px, %.2f s\n", rx, rz, box[2], box[3], now_s() - t0);
        fflush(stdout);
    }
    return failed ? 1 : 0;
}
