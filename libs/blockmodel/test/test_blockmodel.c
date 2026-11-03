/* Resolution tests against a real client jar (the jar's assets can't be
 * checked in), so not part of a plain `make test`:
 *
 *   make test JAR=/path/to/.minecraft/versions/<v>/<v>.jar
 *
 * The expected shapes come from the jar's own models; a version that
 * changes these models would need the expectations updated.
 */
#include "blockmodel.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        failures++; \
    } \
} while (0)

static blockmodel_result_t resolve_z(blockmodel_registry_t *reg, const char *name, const block_faces_t **out) {
    return blockmodel_resolve(reg, name, strlen(name), out);
}

static blockmodel_property_t prop(const char *name, const char *value) {
    blockmodel_property_t p;
    p.name = name; p.name_len = strlen(name);
    p.value = value; p.value_len = strlen(value);
    return p;
}

#define EPS 1e-9
static int near(double a, double b) { return a - b < EPS && b - a < EPS; }

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <path-to-client-jar>\n", argv[0]);
        fprintf(stderr, "(this test needs a real Minecraft client jar - see the comment at the top of this file)\n");
        return 2;
    }

    blockmodel_registry_t *reg;
    blockmodel_result_t r = blockmodel_open(argv[1], &reg);
    CHECK(r == BLOCKMODEL_OK, "jar opens");
    if (r != BLOCKMODEL_OK) { printf("%d test(s) failed\n", failures); return 1; }

    /* stone: cube_all, every face is the same texture, no tint */
    const block_faces_t *stone;
    r = resolve_z(reg, "minecraft:stone", &stone);
    CHECK(r == BLOCKMODEL_OK, "stone resolves");
    if (r == BLOCKMODEL_OK) {
        for (int f = 0; f < FACE_COUNT; f++) {
            CHECK(stone->texture[f] != NULL, "stone has every face resolved");
            CHECK(stone->tint[f] == 0, "stone has no tint on any face");
        }
        CHECK(stone->texture[FACE_UP] == stone->texture[FACE_NORTH], "stone's faces all point at the same cube_all texture");
        if (stone->texture[FACE_UP]) {
            CHECK(stone->texture[FACE_UP]->width == 16 && stone->texture[FACE_UP]->height == 16,
                  "stone texture is 16x16");
        }
    }

    /* oak_log: cube_column - top/bottom differ from the four sides */
    const block_faces_t *oak_log;
    r = resolve_z(reg, "minecraft:oak_log", &oak_log);
    CHECK(r == BLOCKMODEL_OK, "oak_log resolves");
    if (r == BLOCKMODEL_OK) {
        CHECK(oak_log->texture[FACE_UP] != NULL && oak_log->texture[FACE_NORTH] != NULL,
              "oak_log has both a top and a side texture");
        CHECK(oak_log->texture[FACE_UP] != oak_log->texture[FACE_NORTH],
              "oak_log's top (log end-grain) differs from its sides (bark)");
        CHECK(oak_log->texture[FACE_UP] == oak_log->texture[FACE_DOWN], "oak_log's top and bottom match (both end-grain)");
        CHECK(oak_log->texture[FACE_NORTH] == oak_log->texture[FACE_EAST] &&
              oak_log->texture[FACE_EAST] == oak_log->texture[FACE_SOUTH] &&
              oak_log->texture[FACE_SOUTH] == oak_log->texture[FACE_WEST],
              "oak_log's four side faces all match (bark wraps uniformly)");
    }

    /* grass_block: tinted top, plain dirt bottom, side texture untinted */
    const block_faces_t *grass;
    r = resolve_z(reg, "minecraft:grass_block", &grass);
    CHECK(r == BLOCKMODEL_OK, "grass_block resolves");
    if (r == BLOCKMODEL_OK) {
        CHECK(grass->tint[FACE_UP] == 1, "grass_block's top face is marked tinted (biome grass color)");
        CHECK(grass->tint[FACE_DOWN] == 0, "grass_block's bottom face (dirt) is not tinted");
        CHECK(grass->texture[FACE_UP] != NULL && grass->texture[FACE_DOWN] != NULL,
              "grass_block's top and bottom textures both resolve");
        CHECK(grass->texture[FACE_UP] != grass->texture[FACE_DOWN],
              "grass_block's top (grass) and bottom (dirt) are different textures");
    }

    /* caching: resolving the same name twice returns the identical pointer */
    const block_faces_t *stone_again;
    r = resolve_z(reg, "minecraft:stone", &stone_again);
    CHECK(r == BLOCKMODEL_OK && stone_again == stone, "resolving the same block name twice returns the cached pointer");

    /* a block name with no blockstates file at all */
    const block_faces_t *missing;
    r = resolve_z(reg, "minecraft:this_block_does_not_exist", &missing);
    CHECK(r == BLOCKMODEL_ERR_NOT_FOUND, "a nonexistent block name reports NOT_FOUND");

    /* ---- blockmodel_resolve_shape: real per-state geometry ---- */

    /* stone: no properties, cube.json's single full-block element */
    const blockmodel_shape_t *stone_shape;
    r = blockmodel_resolve_shape(reg, "minecraft:stone", strlen("minecraft:stone"), NULL, 0, &stone_shape);
    CHECK(r == BLOCKMODEL_OK, "stone shape resolves");
    if (r == BLOCKMODEL_OK) {
        CHECK(stone_shape->element_count == 1, "stone has exactly one element (full cube)");
        if (stone_shape->element_count == 1) {
            const blockmodel_element_t *el = &stone_shape->elements[0];
            CHECK(near(el->x0, 0) && near(el->y0, 0) && near(el->z0, 0) &&
                  near(el->x1, 16) && near(el->y1, 16) && near(el->z1, 16),
                  "stone's element spans the full block");
            for (int f = 0; f < FACE_COUNT; f++) {
                CHECK(el->faces[f].texture != NULL, "stone's element has every face textured");
            }
        }
    }

    /* oak_slab: type=bottom (slab.json, y:[0,8]) vs type=top (slab_top.json, y:[8,16]) */
    const blockmodel_shape_t *slab_bottom, *slab_top;
    blockmodel_property_t type_bottom[] = {prop("type", "bottom")};
    blockmodel_property_t type_top[] = {prop("type", "top")};
    r = blockmodel_resolve_shape(reg, "minecraft:oak_slab", strlen("minecraft:oak_slab"), type_bottom, 1, &slab_bottom);
    CHECK(r == BLOCKMODEL_OK && slab_bottom->element_count == 1, "oak_slab type=bottom resolves to one element");
    if (r == BLOCKMODEL_OK && slab_bottom->element_count == 1) {
        CHECK(near(slab_bottom->elements[0].y0, 0) && near(slab_bottom->elements[0].y1, 8),
              "oak_slab type=bottom occupies the lower half (y 0-8)");
    }
    r = blockmodel_resolve_shape(reg, "minecraft:oak_slab", strlen("minecraft:oak_slab"), type_top, 1, &slab_top);
    CHECK(r == BLOCKMODEL_OK && slab_top->element_count == 1, "oak_slab type=top resolves to one element");
    if (r == BLOCKMODEL_OK && slab_top->element_count == 1) {
        CHECK(near(slab_top->elements[0].y0, 8) && near(slab_top->elements[0].y1, 16),
              "oak_slab type=top occupies the upper half (y 8-16)");
    }

    /* oak_stairs facing=east (unrotated): stairs.json's two elements, the
     * upper box on the east half, the west riser present. */
    const blockmodel_shape_t *stairs_east;
    blockmodel_property_t stairs_east_props[] = {
        prop("facing", "east"), prop("half", "bottom"), prop("shape", "straight"), prop("waterlogged", "false"),
    };
    r = blockmodel_resolve_shape(reg, "minecraft:oak_stairs", strlen("minecraft:oak_stairs"), stairs_east_props, 4, &stairs_east);
    CHECK(r == BLOCKMODEL_OK && stairs_east->element_count == 2, "oak_stairs facing=east has 2 elements");
    if (r == BLOCKMODEL_OK && stairs_east->element_count == 2) {
        const blockmodel_element_t *upper = &stairs_east->elements[1];
        CHECK(near(upper->x0, 8) && near(upper->x1, 16) && near(upper->z0, 0) && near(upper->z1, 16),
              "oak_stairs facing=east's upper box occupies the east half (x 8-16), full z");
        CHECK(upper->faces[FACE_WEST].texture != NULL,
              "oak_stairs facing=east's upper box has its open west (riser) face present");
    }

    /* oak_stairs facing=north (y=270): the upper box turns to x 0-16,
     * z 0-8, derived by hand from the rotation rule. */
    const blockmodel_shape_t *stairs_north;
    blockmodel_property_t stairs_north_props[] = {
        prop("facing", "north"), prop("half", "bottom"), prop("shape", "straight"), prop("waterlogged", "false"),
    };
    r = blockmodel_resolve_shape(reg, "minecraft:oak_stairs", strlen("minecraft:oak_stairs"), stairs_north_props, 4, &stairs_north);
    CHECK(r == BLOCKMODEL_OK && stairs_north->element_count == 2, "oak_stairs facing=north has 2 elements");
    if (r == BLOCKMODEL_OK && stairs_north->element_count == 2) {
        const blockmodel_element_t *upper = &stairs_north->elements[1];
        CHECK(near(upper->x0, 0) && near(upper->x1, 16) && near(upper->z0, 0) && near(upper->z1, 8) &&
              near(upper->y0, 8) && near(upper->y1, 16),
              "oak_stairs facing=north's upper box rotated to full x (0-16), north half z (0-8)");
    }

    /* oak_fence multipart, north=true only: the post plus the north arm,
     * exactly 3 elements. */
    const blockmodel_shape_t *fence_north;
    blockmodel_property_t fence_props[] = {
        prop("north", "true"), prop("east", "false"), prop("south", "false"), prop("west", "false"),
        prop("waterlogged", "false"),
    };
    r = blockmodel_resolve_shape(reg, "minecraft:oak_fence", strlen("minecraft:oak_fence"), fence_props, 5, &fence_north);
    CHECK(r == BLOCKMODEL_OK && fence_north->element_count == 3,
          "oak_fence north=true alone resolves to 3 elements (post + one 2-element side arm)");

    /* every direction false: just the post, 1 element */
    const blockmodel_shape_t *fence_none;
    blockmodel_property_t fence_none_props[] = {
        prop("north", "false"), prop("east", "false"), prop("south", "false"), prop("west", "false"),
        prop("waterlogged", "false"),
    };
    r = blockmodel_resolve_shape(reg, "minecraft:oak_fence", strlen("minecraft:oak_fence"), fence_none_props, 5, &fence_none);
    CHECK(r == BLOCKMODEL_OK && fence_none->element_count == 1,
          "oak_fence with no attached sides resolves to just the post (1 element)");

    /* multipart shape caching: same state resolved twice returns the
     * identical cached pointer */
    const blockmodel_shape_t *fence_north_again;
    r = blockmodel_resolve_shape(reg, "minecraft:oak_fence", strlen("minecraft:oak_fence"), fence_props, 5, &fence_north_again);
    CHECK(r == BLOCKMODEL_OK && fence_north_again == fence_north,
          "resolving the same block+properties twice returns the cached shape pointer");

    blockmodel_close(reg);

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
