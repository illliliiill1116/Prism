/* Self-contained tests:
 * 1. The spec's "hello world" vector: a compound "hello world" holding
 *    "name" -> "Bananrama".
 * 2. A round trip through the synthetic world generator: counts, scalar
 *    fields, and int/long arrays after the in-place byte swap.
 */
#include "nbt.h"
#include "synth.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        failures++; \
    } \
} while (0)

static void test_hello_world(void) {
    static const uint8_t vector[] = {
        0x0A, 0x00, 0x0B, 'h','e','l','l','o',' ','w','o','r','l','d',
        0x08, 0x00, 0x04, 'n','a','m','e',
        0x00, 0x09, 'B','a','n','a','n','r','a','m','a',
        0x00,
    };
    uint8_t buf[sizeof(vector)];
    memcpy(buf, vector, sizeof(vector));

    nbt_pool_t pool;
    nbt_pool_init(&pool);
    nbt_tag_t *root;
    nbt_result_t r = nbt_parse(&pool, buf, sizeof(buf), &root);

    CHECK(r == NBT_OK, "hello world parses without error");
    CHECK(root->type == NBT_COMPOUND, "root is a compound");
    CHECK(root->name_len == 11 && memcmp(root->name, "hello world", 11) == 0, "root name is 'hello world'");
    CHECK(root->child_count == 1, "root has exactly one child");

    nbt_tag_t *name_tag = nbt_compound_getz(&pool, root, "name");
    CHECK(name_tag != NULL, "child 'name' exists");
    if (name_tag) {
        CHECK(name_tag->type == NBT_STRING, "'name' is a string tag");
        CHECK(name_tag->string.len == 9 && memcmp(name_tag->string.data, "Bananrama", 9) == 0,
              "'name' value is 'Bananrama'");
    }

    nbt_pool_free(&pool);
}

static void test_synthetic_round_trip(void) {
    const int num_chunks = 17; /* deliberately not a round number */
    uint8_t *master;
    size_t len = nbt_synth_build_world(0xC0FFEE, num_chunks, &master);
    CHECK(len > 0, "synthetic world serializes to a non-empty buffer");

    nbt_pool_t pool;
    nbt_pool_init(&pool);
    nbt_tag_t *root;
    nbt_result_t r = nbt_parse(&pool, master, len, &root);
    CHECK(r == NBT_OK, "synthetic world parses without error");
    if (r != NBT_OK) { free(master); return; }

    CHECK(root->type == NBT_COMPOUND, "root is a compound");
    CHECK(root->name_len == 5 && memcmp(root->name, "level", 5) == 0, "root name is 'level'");

    nbt_tag_t *data = nbt_compound_getz(&pool, root, "Data");
    CHECK(data != NULL && data->type == NBT_COMPOUND, "'Data' compound present");
    if (data) {
        nbt_tag_t *dv = nbt_compound_getz(&pool, data, "DataVersion");
        CHECK(dv && dv->type == NBT_INT && dv->v_int == 3465, "DataVersion round-trips as 3465");
        nbt_tag_t *name = nbt_compound_getz(&pool, data, "LevelName");
        CHECK(name && name->type == NBT_STRING &&
              name->string.len == (int32_t)strlen("prism-benchmark-world") &&
              memcmp(name->string.data, "prism-benchmark-world", name->string.len) == 0,
              "LevelName round-trips");
    }

    nbt_tag_t *chunks = nbt_compound_getz(&pool, root, "Chunks");
    CHECK(chunks != NULL && chunks->type == NBT_LIST, "'Chunks' list present");
    CHECK(chunks && chunks->child_count == (uint32_t)num_chunks, "chunk count matches what was generated");

    if (chunks && chunks->child_count > 0) {
        nbt_tag_t *chunk0 = nbt_child_at(&pool, chunks, 0);
        CHECK(chunk0->type == NBT_COMPOUND, "chunk element is a compound");

        nbt_tag_t *sections = nbt_compound_getz(&pool, chunk0, "Sections");
        CHECK(sections && sections->child_count == 24, "each chunk has 24 sections");

        if (sections && sections->child_count > 0) {
            nbt_tag_t *sec0 = nbt_child_at(&pool, sections, 0);
            nbt_tag_t *y = nbt_compound_getz(&pool, sec0, "Y");
            CHECK(y && y->type == NBT_BYTE && y->v_byte == -4, "first section Y is -4");

            nbt_tag_t *blockstates = nbt_compound_getz(&pool, sec0, "BlockStates");
            CHECK(blockstates && blockstates->type == NBT_LONG_ARRAY && blockstates->long_array.len == 256,
                  "BlockStates long-array has 256 entries");

            nbt_tag_t *biomes = nbt_compound_getz(&pool, sec0, "Biomes");
            CHECK(biomes && biomes->type == NBT_BYTE_ARRAY && biomes->byte_array.len == 256,
                  "Biomes byte-array has 256 entries");
        }
    }

    uint64_t total = nbt_count_tags(&pool, root);
    /* 1 (root) + Data(1+4) + Chunks(1) + num_chunks * (1 chunk + 4 scalars + 1 Sections + 24*(1+3)) */
    uint64_t expected = 1 + 5 + 1 + (uint64_t)num_chunks * (1 + 4 + 1 + 24 * 4);
    CHECK(total == expected, "total tag count matches hand-computed expectation");
    if (total != expected) {
        fprintf(stderr, "  got %llu, expected %llu\n", (unsigned long long)total, (unsigned long long)expected);
    }

    nbt_pool_free(&pool);
    free(master);
}

int main(void) {
    test_hello_world();
    test_synthetic_round_trip();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
