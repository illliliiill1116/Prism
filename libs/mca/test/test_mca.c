/* Region reader tests on a synthetic .mca: the location table (present vs
 * never generated), parsing one chunk through zlib, and iteration order
 * and count. */
#define _POSIX_C_SOURCE 200809L

#include "mca.h"
#include "synth_region.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        failures++; \
    } \
} while (0)

static int count_cb(int local_x, int local_z, nbt_pool_t *pool, nbt_tag_t *root, void *ud) {
    (void)local_x; (void)local_z;
    int *seen = (int *)ud;
    (*seen)++;
    /* touch the tree to make sure it's actually usable inside the callback */
    uint64_t tags = nbt_count_tags(pool, root);
    if (tags == 0) failures++;
    return 0;
}

int main(void) {
    char path[] = "/tmp/prism_mca_test_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp succeeds");
    if (fd >= 0) close(fd);

    const int num_chunks = 100; /* partially fills the 32x32 region */
    int wr = mca_synth_write_region(path, 0xABCDEF, num_chunks);
    CHECK(wr == 0, "synthetic region file writes successfully");

    mca_region_t region;
    mca_result_t r = mca_region_open(path, &region);
    CHECK(r == MCA_OK, "region file opens");
    if (r != MCA_OK) { remove(path); return 1; }

    /* chunks were laid out row-major starting at (0,0): local (3,1) is
     * chunk index 1*32+3 = 35, which is < 100, so it should be present. */
    mca_chunk_loc_t loc;
    r = mca_region_get_loc(&region, 3, 1, &loc);
    CHECK(r == MCA_OK, "get_loc for a present chunk succeeds");
    CHECK(mca_chunk_present(&loc), "chunk (3,1) is present");

    /* index 31*32+31 = 1023, well past num_chunks=100, must be absent. */
    r = mca_region_get_loc(&region, 31, 31, &loc);
    CHECK(r == MCA_OK, "get_loc for an absent chunk succeeds");
    CHECK(!mca_chunk_present(&loc), "chunk (31,31) is absent");

    r = mca_region_get_loc(&region, 32, 0, &loc);
    CHECK(r == MCA_ERR_BOUNDS, "out-of-range local coords are rejected");

    nbt_pool_t pool;
    nbt_tag_t *root;
    r = mca_region_parse_chunk(&region, 3, 1, &pool, &root);
    CHECK(r == MCA_OK, "parsing a present chunk succeeds");
    if (r == MCA_OK) {
        CHECK(root->type == NBT_COMPOUND, "chunk root is a compound");
        nbt_tag_t *xpos = nbt_compound_getz(&pool, root, "xPos");
        nbt_tag_t *zpos = nbt_compound_getz(&pool, root, "zPos");
        CHECK(xpos && xpos->type == NBT_INT && xpos->v_int == 3, "xPos round-trips as 3");
        CHECK(zpos && zpos->type == NBT_INT && zpos->v_int == 1, "zPos round-trips as 1");

        nbt_tag_t *sections = nbt_compound_getz(&pool, root, "Sections");
        CHECK(sections && sections->child_count == 24, "chunk has 24 sections");
        nbt_pool_free(&pool);
    }

    r = mca_region_parse_chunk(&region, 31, 31, &pool, &root);
    CHECK(r == MCA_ERR_EMPTY, "parsing an absent chunk returns MCA_ERR_EMPTY");

    int seen = 0;
    int failed = 0;
    r = mca_region_for_each_chunk(&region, count_cb, &seen, &failed);
    CHECK(r == MCA_OK, "for_each_chunk completes");
    CHECK(seen == num_chunks, "for_each_chunk visits exactly num_chunks chunks");
    CHECK(failed == 0, "no chunks fail to parse");

    mca_region_close(&region);
    remove(path);

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
