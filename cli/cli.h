/* The commands of the prism tool (cli/main.c picks one). */
#ifndef PRISM_CLI_H
#define PRISM_CLI_H

#include "prism.h"
#include <sys/stat.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

static inline double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Meshes come and go (iso's windows, the bake's bands): large blocks from
 * their own mappings, given back when freed (glibc would raise its
 * threshold past them after the first, and keep what they leave in its
 * arenas). */
static inline void big_allocs_mapped(void) {
#ifdef __GLIBC__
    mallopt(M_MMAP_THRESHOLD, 1 << 20);
#endif
}

/* The overworld's region files of a world's folder (26.x keeps them in
 * dimensions/minecraft/overworld/region, older worlds in region), or arg
 * itself when it is the region folder. */
static inline void region_dir(const char *arg, char *out, size_t len) {
    static const char *sub[] = { "dimensions/minecraft/overworld/region", "region" };
    struct stat st;
    for (int i = 0; i < 2; i++) {
        snprintf(out, len, "%s/%s", arg, sub[i]);
        if (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) return;
    }
    snprintf(out, len, "%s", arg);
}

int cmd_bake(int argc, char **argv);
int bake_region(const char *rdir, int c0x, int c0z, const char *out, int nthreads, int want_glb);
int write_atomic(const char *path, const void *p, size_t n);
int cmd_map(int argc, char **argv);
int cmd_iso(int argc, char **argv);

#endif
