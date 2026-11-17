/*
 * prism overview: regions as small isometric pictures (iso.c's drawing, in
 * its projection), quick, for picking an area to bake (the app's picker).
 *
 *   prism overview <client.jar> <world or region dir> <out dir> <rx> <rz> [<rx> <rz>...]
 *                  [--scale px] [--ymin y] [--threads n]
 *
 * Each region is loaded once, as a bake's window, and drawn alone (cut at
 * its edges and at ymin, as iso cuts its area), with the sun but no cast
 * shadows and the least bounced light. It writes <out>/r.<x>.<z>.png and
 * r.<x>.<z>.json: {"scale", "ymin", "x", "y", "w", "h"}, the picture's
 * corner in projected pixels (iso_project) and its size, w 0 and no
 * picture when the region has nothing in it. Pictures of neighbours drawn
 * in that order by rx + rz, each at its corner, make the world.
 */
#include "cli.h"

#define BAND 8 /* chunk rows drawn at once */
#define BACK 4 /* and meshed behind them: what their water looks down to (iso.c's BACK) */

static int write_text(const char *path, const char *s) { return write_atomic(path, s, strlen(s)); }

int cmd_overview(int argc, char **argv) {
    const char *usage = "usage: prism overview <client.jar> <world or region dir> <out dir> <rx> <rz> [<rx> <rz>...]\n"
                        "                      [--scale px] [--ymin y] [--threads n]\n";
    if (argc < 6) { fputs(usage, stderr); return 2; }
    const char *jar = argv[1], *out = argv[3];
    char rdir[4096];
    region_dir(argv[2], rdir, sizeof rdir);
    iso_opts_t o = { .scale = 2, .ss = 1, .shadows = 0, .threads = plat_ncpu(), .sun = 136.4f };
    int ymin = 0, n = 0;
    static int regs[4096][2];
    for (int i = 4; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (a[0] == '-' && a[1] == '-') {
            if (!v) { fputs(usage, stderr); return 2; }
            if (!strcmp(a, "--scale")) o.scale = atoi(v);
            else if (!strcmp(a, "--ymin")) ymin = atoi(v);
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
    if (!n || o.scale < 1 || o.scale > 16 || o.threads < 1 || ymin < YMIN || ymin >= YMIN + GH) {
        fprintf(stderr, "--scale is 1..16, --ymin -64..319, --threads 1 or more, and a region at least\n");
        return 2;
    }
    big_allocs_mapped();
    if (prism_open(jar)) return 1;
    plat_mkdir(out);
    light_one_sun(o.sun * 3.14159265f / 180);
    light_defer = 1;
    lc_bits = fc_bits = 21; /* a band's corners */
    set_bounce_rays(1);
    int failed = 0;
    for (int k = 0; k < n; k++) {
        double t0 = now_s();
        int rx = regs[k][0], rz = regs[k][1];
        int x0 = rx * REG, z0 = rz * REG, x1 = x0 + REG - 1, z1 = z0 + REG - 1;
        char path[4096], json[256];
        canvas_t atlas;
        long lit, emitters;
        window_load(rdir, rx * 32, rz * 32, &atlas, &lit, &emitters);
        window_clear_outside(x0, z0, x1, z1, ymin);
        /* a band of chunk rows at a time, back (-z) to front: only a
         * band's quads are held. Level 0: level 1's far plants and leaves
         * show up wrong this close (hanging vines as blades) */
        lod_mode = 0;
        static uint8_t empty[32 * 32];
        memcpy(empty, mesh_skip, sizeof empty);
        long box[4] = { 0, 0, 0, 0 }, quads = 0;
        iso_image_t img;
        int made = !iso_image_init(&img, o.scale, 0, x0, z0, x1, z1, ymin), bad = !made;
        for (int b0 = 0; b0 < 32 && chunks_loaded && !bad; b0 += BAND) {
            int any = 0;
            for (int c = b0 * 32; c < (b0 + BAND) * 32; c++) any |= !empty[c];
            for (int c = 0; c < 32 * 32; c++) {
                mesh_skip[c] = empty[c] || c / 32 < b0 - BACK || c / 32 >= b0 + BAND;
            }
            if (!any) continue;
            static size_t q0[MAT_COUNT][NTILES + 1];
            mbuf_t merged[MAT_COUNT];
            long q = mesh_pass(o.threads, merged, q0);
            int core[4] = { x0, z0 + b0 * 16, x1, z0 + (b0 + BAND) * 16 - 1 };
            if (q && iso_draw(&img, merged, &atlas, &o, core)) bad = 1;
            mb_free(merged);
            light_caches_clear();
            quads += q;
        }
        memset(mesh_skip, 0, sizeof mesh_skip);
        snprintf(path, sizeof path, "%s/r.%d.%d.png", out, rx, rz);
        if (bad || (quads && iso_image_write(&img, path, o.threads, box))) {
            fprintf(stderr, "r.%d.%d: can't draw it\n", rx, rz);
            failed++;
            box[2] = 0;
        }
        if (made) iso_image_free(&img);
        canvas_free(&atlas);
        snprintf(json, sizeof json, "{\"scale\":%d,\"ymin\":%d,\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld}\n", o.scale, ymin,
                 box[0], box[1], box[2], box[3]);
        snprintf(path, sizeof path, "%s/r.%d.%d.json", out, rx, rz);
        if (write_text(path, json)) { fprintf(stderr, "can't write %s\n", path); return 1; }
        printf("r.%d.%d: %ld x %ld px, %.2f s\n", rx, rz, box[2], box[3], now_s() - t0);
        fflush(stdout);
    }
    return failed ? 1 : 0;
}
