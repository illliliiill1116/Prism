/*
 * prism iso: a world area as an isometric picture, drawn on the CPU (see
 * src/isodraw.c), in the 2.5D renderer's projection.
 *
 *   prism iso <client.jar> <world or region dir> <x0> <z0> <x1> <z1> <out.png>
 *             [--sun deg] [--scale px] [--ss n] [--ymin y] [--bounce-rays n] [--threads n] [--noshadow]
 *
 * x0, z0 and x1, z1 are opposite corners, world blocks, both inside. The
 * area goes in windows of the bake's size, each loaded with the world
 * around it, so the light and the shadows falling in are the world's; the
 * ground is cut open at the area's edge (and at --ymin, below which
 * nothing is drawn). Each window's core, CORE chunks
 * square, is what it draws; BACK chunks before it (-x, -z) are what the
 * core's water at its back edge looks down to, and the chunk after it
 * (+x, +z) casts its near shadows (NEAR1 blocks, a fence's 12).
 */
#include "cli.h"

#define BACK 4
#define CORE 27
_Static_assert(BACK + CORE + 1 == REG / 16, "a window is BACK + CORE + 1 chunks");

static int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

int cmd_iso(int argc, char **argv) {
    if (argc < 8) {
        fprintf(stderr, "usage: prism iso <client.jar> <world or region dir> <x0> <z0> <x1> <z1> <out.png> [--sun deg] [--scale px]\n"
                        "                 [--ss n] [--ymin y] [--bounce-rays n] [--threads n] [--noshadow]\n");
        return 2;
    }
    const char *jar = argv[1], *out = argv[7];
    char rdir[4096];
    region_dir(argv[2], rdir, sizeof rdir);
    int x0 = atoi(argv[3]), z0 = atoi(argv[4]), x1 = atoi(argv[5]), z1 = atoi(argv[6]);
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (z0 > z1) { int t = z0; z0 = z1; z1 = t; }
    iso_opts_t o = { .scale = 16, .ss = 1, .shadows = 1, .threads = 16, .sun = 136.4f };
    int ymin = YMIN, rays = 16;
    for (int i = 8; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--noshadow") == 0) { o.shadows = 0; continue; }
        if (!v) { fprintf(stderr, "%s needs a value\n", a); return 2; }
        if (strcmp(a, "--sun") == 0) o.sun = (float)atof(v);
        else if (strcmp(a, "--scale") == 0) o.scale = atoi(v);
        else if (strcmp(a, "--ss") == 0) o.ss = atoi(v);
        else if (strcmp(a, "--threads") == 0) o.threads = atoi(v);
        else if (strcmp(a, "--ymin") == 0) ymin = atoi(v);
        else if (strcmp(a, "--bounce-rays") == 0) rays = atoi(v);
        else { fprintf(stderr, "unknown option %s\n", a); return 2; }
        i++;
    }
    if (o.scale < 1 || o.scale > 64 || o.ss < 1 || o.ss > 4 || o.threads < 1 || ymin < YMIN || ymin >= YMIN + GH) {
        fprintf(stderr, "--scale is 1..64, --ss 1..4, --ymin -64..319, --threads 1 or more\n");
        return 2;
    }
    big_allocs_mapped();
    double t0 = now_s();
    iso_image_t img;
    if (iso_image_init(&img, o.scale, x0, z0, x1, z1, ymin)) {
        fprintf(stderr, "the picture would be too large: a smaller area or --scale\n");
        return 1;
    }
    if (prism_open(jar)) return 1;
    /* only the sun the picture has (isodraw.c's place_sun: the moon's arc is the sun's) */
    float deg = fmodf(fmodf(o.sun, 360) + 360, 360);
    light_one_sun((deg <= 180 ? deg : deg - 180) * 3.14159265f / 180);
    /* and only the quads that show: isodraw.c lights them (light_quad) */
    light_defer = 1;
    set_bounce_rays(rays);
    int kx0 = floor_div(x0, 16), kz0 = floor_div(z0, 16);
    int nx = (floor_div(x1, 16) - kx0) / CORE + 1, nz = (floor_div(z1, 16) - kz0) / CORE + 1;
    printf("iso: %d x %d blocks, %d window%s, up to %d x %d px\n", x1 - x0 + 1, z1 - z0 + 1, nx * nz,
           nx * nz > 1 ? "s" : "", img.w, img.h);
    double t_load = 0, t_mesh = 0, t_draw = 0;
    int done = 0;
    /* back to front: by x + z (see isodraw.c) */
    for (int s = 0; s <= nx + nz - 2; s++)
        for (int i = 0; i < nx; i++) {
            int j = s - i;
            if (j < 0 || j >= nz) continue;
            int cx = kx0 + i * CORE, cz = kz0 + j * CORE;
            int core[4] = { cx * 16, cz * 16, (cx + CORE) * 16 - 1, (cz + CORE) * 16 - 1 };
            if (core[0] < x0) core[0] = x0;
            if (core[1] < z0) core[1] = z0;
            if (core[2] > x1) core[2] = x1;
            if (core[3] > z1) core[3] = z1;
            double a = now_s();
            canvas_t atlas;
            long lit, emitters;
            window_load(rdir, cx - BACK, cz - BACK, &atlas, &lit, &emitters);
            window_clear_outside(x0, z0, x1, z1, ymin);
            double b = now_s();
            lod_mode = 0;
            static size_t q0[MAT_COUNT][NTILES + 1];
            mbuf_t merged[MAT_COUNT];
            long quads = chunks_loaded ? mesh_pass(o.threads, merged, q0) : 0;
            double c = now_s();
            if (quads && iso_draw(&img, merged, &atlas, &o, core)) return 1;
            if (chunks_loaded) mb_free(merged);
            canvas_free(&atlas);
            double d = now_s();
            t_load += b - a; t_mesh += c - b; t_draw += d - c;
            printf("iso: window %d/%d at chunk %d,%d: %ld chunks, %ld quads, %.1f s\n", ++done, nx * nz, cx, cz,
                   chunks_loaded, quads, d - a);
            fflush(stdout);
        }
    long box[4];
    double t_w = now_s();
    if (iso_image_write(&img, out, o.threads, box)) return 1;
    iso_image_free(&img);
    double t_end = now_s();
    printf("iso: %s, %ld x %ld px, its corner at projected pixel %ld, %ld (%d px a block)\n", out, box[2], box[3],
           box[0], box[1], o.scale);
    printf("time: load %.2f s, mesh + bake %.2f s, draw %.2f s, write %.2f s (%d threads), total %.2f s\n",
           t_load, t_mesh, t_draw, t_end - t_w, o.threads, t_end - t0);
    return 0;
}
