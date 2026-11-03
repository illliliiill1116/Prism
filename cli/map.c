/*
 * prism map: a whole world baked into a map for the viewer (?map=), and on
 * later runs baked again only where the world changed.
 *
 *   prism map <client.jar> <world or region dir> <out dir> [--threads n]
 *
 * Each region file goes into <out>/r.<x>.<z>/ (its bake's log in
 * r.<x>.<z>.log), and <out>/map.json lists them all with their tile counts
 * and the sums of their tiles' tx, tz (where the viewer starts), and gives
 * the world's water surface: of the regions' own, the one most columns
 * have (where the viewer's ground mist lies). A region is
 * baked again when there is no bake of it in this format, prism is newer
 * than its bake, its region file changed since, or a neighbour's chunk
 * within MARGIN of it did (its bake reads those too), by the chunk
 * timestamps in the neighbour's region file. A bake goes to a new folder
 * that replaces the old one when it is done, so a map being served is never
 * half baked (a viewer that read the old index.json notices, by the tiles'
 * Last-Modified, and loads the region again).
 */
#include "cli.h"
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <sys/wait.h>
#include <unistd.h>

#define BAKE_MB 1800 /* a region's bake at its peak, about (IHOU's dense build: 1.8 GB) */

typedef struct { int rx, rz; } reg_t;

/* MemAvailable, in MB, or -1 */
static long mem_available_mb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    long kb = -1;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
    if (f) fclose(f);
    return kb < 0 ? -1 : kb / 1024;
}

static int by_name(const void *a, const void *b) {
    const reg_t *p = a, *q = b;
    return p->rx != q->rx ? (p->rx < q->rx ? -1 : 1) : (p->rz > q->rz) - (p->rz < q->rz);
}

static int rm_one(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw;
    return remove(path);
}

static void rm_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) == 0) nftw(path, rm_one, 16, FTW_DEPTH | FTW_PHYS);
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    if (p) { p[n] = 0; *len = (size_t)n; }
    return p;
}

/* What a region's index.json says: its format, when it was baked, its
 * tile count and the sums of the tiles' tx, tz, and its water surface and
 * the columns that have it. 0 if there is none. */
typedef struct { int format; long long baked; long tiles, tx, tz; int sea; long sea_cols; } bake_info_t;

static int bake_info(const char *dir, bake_info_t *bi) {
    char path[4096];
    snprintf(path, sizeof path, "%s/index.json", dir);
    size_t len;
    char *text = read_file(path, &len);
    *bi = (bake_info_t){ 0 };
    if (!text) return 0;
    json_pool_t pool;
    json_pool_init(&pool);
    json_value_t *root, *v;
    int ok = json_parse(&pool, text, len, &root) == JSON_OK && root->type == JSON_OBJECT;
    if (ok && (v = json_object_getz(&pool, root, "format")) && v->type == JSON_NUMBER) bi->format = (int)v->number;
    if (ok && (v = json_object_getz(&pool, root, "baked")) && v->type == JSON_NUMBER) bi->baked = (long long)v->number;
    if (ok && (v = json_object_getz(&pool, root, "sea")) && v->type == JSON_ARRAY && json_array_count(v) == 2) {
        bi->sea = (int)json_array_at(&pool, v, 0)->number;
        bi->sea_cols = (long)json_array_at(&pool, v, 1)->number;
    }
    if (ok && (v = json_object_getz(&pool, root, "tiles")) && v->type == JSON_ARRAY) {
        for (uint32_t i = 0; i < json_array_count(v); i++) {
            json_value_t *t = json_array_at(&pool, v, i);
            if (t->type != JSON_ARRAY || json_array_count(t) < 2) continue;
            bi->tiles++;
            bi->tx += (long)json_array_at(&pool, t, 0)->number;
            bi->tz += (long)json_array_at(&pool, t, 1)->number;
        }
    }
    json_pool_free(&pool);
    free(text);
    return ok;
}

/* Whether region file n (at nx, nz) has a chunk saved at or after since
 * within MARGIN of region rx, rz. */
static int edge_changed(const char *rdir, int nx, int nz, int rx, int rz, long long since) {
    char path[4096];
    snprintf(path, sizeof path, "%s/r.%d.%d.mca", rdir, nx, nz);
    mca_region_t r;
    if (mca_region_open(path, &r) != MCA_OK) { mca_region_close(&r); return 0; }
    const int m = (MARGIN + 15) / 16;
    int changed = 0;
    for (int lz = 0; lz < 32 && !changed; lz++)
        for (int lx = 0; lx < 32 && !changed; lx++) {
            int gx = nx * 32 + lx - rx * 32, gz = nz * 32 + lz - rz * 32;
            if (gx < -m || gx >= 32 + m || gz < -m || gz >= 32 + m) continue;
            mca_chunk_loc_t loc;
            if (mca_region_get_loc(&r, lx, lz, &loc) == MCA_OK && mca_chunk_present(&loc) && loc.timestamp >= since)
                changed = 1;
        }
    mca_region_close(&r);
    return changed;
}

static int has_region(const reg_t *regs, int n, int rx, int rz) {
    for (int i = 0; i < n; i++) if (regs[i].rx == rx && regs[i].rz == rz) return 1;
    return 0;
}

/* Why region i needs baking, or NULL when its bake is up to date. */
static const char *stale(const char *rdir, const char *dir, const reg_t *regs, int n, int i, long long exe_mtime,
                         char *why, size_t whylen) {
    bake_info_t bi;
    if (!bake_info(dir, &bi) || bi.format != MAP_FORMAT || !bi.baked) return "new";
    if (exe_mtime >= bi.baked) return "prism is newer";
    char path[4096];
    struct stat st;
    snprintf(path, sizeof path, "%s/r.%d.%d.mca", rdir, regs[i].rx, regs[i].rz);
    if (stat(path, &st) == 0 && (long long)st.st_mtime >= bi.baked) return "changed";
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = regs[i].rx + dx, nz = regs[i].rz + dz;
            if ((dx || dz) && has_region(regs, n, nx, nz) && edge_changed(rdir, nx, nz, regs[i].rx, regs[i].rz, bi.baked)) {
                snprintf(why, whylen, "r.%d.%d changed at the edge", nx, nz);
                return why;
            }
        }
    return NULL;
}

/* The warnings in a bake's log (unreadable chunks: see window_load), shown
 * as it finishes. */
static void show_warnings(const char *log) {
    FILE *f = fopen(log, "r");
    char line[512];
    while (f && fgets(line, sizeof line, f))
        if (strncmp(line, "warning: ", 9) == 0) printf("  %s", line);
    if (f) fclose(f);
}

/* Bakes region rx, rz into dir in a child process (each bake starts from
 * the state prism_open left, and gives its memory back when done), by way
 * of dir.new. Its log goes to log. */
static int bake_one(const char *rdir, int rx, int rz, const char *dir, const char *log, int nthreads) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.new", dir);
    rm_tree(tmp);
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        if (!freopen(log, "w", stdout)) _exit(1);
        int rc = bake_region(rdir, rx * 32, rz * 32, tmp, nthreads, 0);
        fflush(stdout);
        _exit(rc ? 1 : 0);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) { rm_tree(tmp); return 1; }
    /* the old bake out of the way and the new one in: two renames, so the
       region is missing only between them, not while the old one is deleted */
    char old[4096];
    snprintf(old, sizeof old, "%s.old", dir);
    rm_tree(old);
    if (rename(dir, old) && errno != ENOENT) { perror(dir); rm_tree(tmp); return 1; }
    if (rename(tmp, dir)) { perror(dir); return 1; }
    rm_tree(old);
    return 0;
}

int cmd_map(int argc, char **argv) {
    const char *usage = "usage: prism map <client.jar> <world or region dir> <out dir> [--threads n]\n";
    if (argc < 4) { fputs(usage, stderr); return 2; }
    const char *jar = argv[1], *out = argv[3];
    int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) nthreads = atoi(argv[++i]);
        else { fprintf(stderr, "unknown option %s\n%s", argv[i], usage); return 2; }
    }
    if (nthreads < 1) { fprintf(stderr, "threads: 1 or more\n"); return 2; }

    char rdir[4096];
    struct stat st;
    region_dir(argv[2], rdir, sizeof rdir);
    DIR *d = opendir(rdir);
    if (!d) { fprintf(stderr, "can't read %s\n", rdir); return 1; }
    reg_t *regs = NULL;
    int n = 0, cap = 0;
    for (struct dirent *e; (e = readdir(d));) {
        int rx, rz, len = 0;
        if (sscanf(e->d_name, "r.%d.%d.mca%n", &rx, &rz, &len) != 2 || e->d_name[len]) continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", rdir, e->d_name);
        if (stat(path, &st) || st.st_size == 0) continue;
        if (n == cap && !(regs = realloc(regs, (size_t)(cap = cap ? cap * 2 : 64) * sizeof *regs))) { fprintf(stderr, "oom\n"); return 1; }
        regs[n++] = (reg_t){ rx, rz };
    }
    closedir(d);
    if (!n) { fprintf(stderr, "no region files in %s\n", rdir); return 1; }
    qsort(regs, (size_t)n, sizeof *regs, by_name);

    if (mkdir(out, 0755) && errno != EEXIST) { perror(out); return 1; }
    /* player heads wear the skins in PRISM_SKINS, by default skins/ next to the map */
    if (!getenv("PRISM_SKINS")) {
        char skins[4096];
        snprintf(skins, sizeof skins, "%s/../skins", out);
        setenv("PRISM_SKINS", skins, 1);
    }
    long long exe_mtime = stat("/proc/self/exe", &st) == 0 ? (long long)st.st_mtime : 0;
    long avail = mem_available_mb();
    if (avail >= 0 && avail < BAKE_MB)
        fprintf(stderr, "warning: %ld MB of memory available, and baking a region can take up to about %d MB\n",
                avail, BAKE_MB);
    big_allocs_mapped();
    if (prism_open(jar)) return 1;

    int failed = 0, baked = 0;
    str_t list = { 0 };
    /* the regions' water surfaces and how many columns have each */
    int *sea_y = calloc((size_t)n + 1, sizeof *sea_y);
    long *sea_n = calloc((size_t)n + 1, sizeof *sea_n);
    int nsea = 0;
    for (int i = 0; i < n; i++) {
        char dir[4096], log[4096], why[64];
        snprintf(dir, sizeof dir, "%s/r.%d.%d", out, regs[i].rx, regs[i].rz);
        snprintf(log, sizeof log, "%s.log", dir);
        const char *reason = stale(rdir, dir, regs, n, i, exe_mtime, why, sizeof why);
        if (!reason) printf("r.%d.%d: up to date\n", regs[i].rx, regs[i].rz);
        else {
            printf("r.%d.%d: baking (%s)... ", regs[i].rx, regs[i].rz, reason);
            fflush(stdout);
            double t0 = now_s();
            if (bake_one(rdir, regs[i].rx, regs[i].rz, dir, log, nthreads)) {
                printf("failed, see %s\n", log);
                failed++;
            } else {
                printf("%.1f s\n", now_s() - t0);
                baked++;
            }
            show_warnings(log);
        }
        bake_info_t bi;
        bake_info(dir, &bi);
        sappend(&list, "%s{\"rx\":%d,\"rz\":%d,\"dir\":\"r.%d.%d\",\"tiles\":%ld,\"tx\":%ld,\"tz\":%ld}", i ? "," : "",
                regs[i].rx, regs[i].rz, regs[i].rx, regs[i].rz, bi.tiles, bi.tx, bi.tz);
        if (bi.sea_cols > 0 && sea_y && sea_n) {
            int k = 0;
            while (k < nsea && sea_y[k] != bi.sea) k++;
            if (k == nsea) { sea_y[nsea] = bi.sea; sea_n[nsea++] = 0; }
            sea_n[k] += bi.sea_cols;
        }
    }
    int sea = -1;
    for (int k = 0; k < nsea; k++)
        if (sea < 0 || sea_n[k] > sea_n[sea]) sea = k;

    char path[4096];
    snprintf(path, sizeof path, "%s/map.json", out);
    str_t mj = { 0 };
    sappend(&mj, "{\"format\":%d,", MAP_FORMAT);
    if (sea >= 0) sappend(&mj, "\"sea\":%d,", sea_y[sea]);
    sappend(&mj, "\"regions\":[%s]}\n", list.p ? list.p : "");
    free(sea_y);
    free(sea_n);
    if (write_atomic(path, mj.p, mj.n)) { fprintf(stderr, "can't write %s\n", path); return 1; }
    free(mj.p);
    free(list.p);
    free(regs);
    printf("%d regions, %d baked%s\n", n, baked, failed ? ", some failed" : "");
    return failed ? 1 : 0;
}
