/*
 * skinlist <region dir> - prints the skin hash of every player head in the
 * region files, once each, for fetch_skins.sh to download. A head's
 * block entity carries its skin as profile.properties[name=textures], a
 * base64 JSON whose SKIN url is http://textures.minecraft.net/texture/<hash>;
 * any other url is skipped, so the world can't point the download elsewhere.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mca.h"
#include "nbt.h"
#include "world/skin.h"

static char (*seen)[SKIN_HASH_MAX + 1];
static size_t nseen, cseen;

static int cb(int lx, int lz, nbt_pool_t *p, nbt_tag_t *root, void *u) {
    (void)lx; (void)lz; (void)u;
    nbt_tag_t *be = nbt_compound_getz(p, root, "block_entities");
    for (uint32_t i = 0; be && i < be->child_count; i++) {
        char hash[SKIN_HASH_MAX + 1];
        if (!skin_hash_of(p, nbt_child_at(p, be, i), hash)) continue;
        size_t k = 0;
        while (k < nseen && strcmp(seen[k], hash)) k++;
        if (k < nseen) continue;
        if (nseen == cseen) {
            cseen = cseen ? cseen * 2 : 64;
            seen = realloc(seen, cseen * sizeof *seen);
            if (!seen) { perror("realloc"); exit(1); }
        }
        strcpy(seen[nseen++], hash);
        puts(hash);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <region dir>\n", argv[0]); return 2; }
    DIR *d = opendir(argv[1]);
    if (!d) { perror(argv[1]); return 1; }
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t n = strlen(de->d_name);
        if (n < 4 || strcmp(de->d_name + n - 4, ".mca")) continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", argv[1], de->d_name);
        mca_region_t r;
        if (mca_region_open(path, &r) == MCA_OK) mca_region_for_each_chunk(&r, cb, NULL, NULL);
        mca_region_close(&r);
    }
    closedir(d);
    return 0;
}
