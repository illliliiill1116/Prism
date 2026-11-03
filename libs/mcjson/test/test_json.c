/* Parser tests on literal JSON: value types, nesting, escapes (including
 * \uXXXX), and the shapes blockstate and model files use. */
#include "json.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        failures++; \
    } \
} while (0)

static json_value_t *parse_ok(json_pool_t *pool, const char *text) {
    json_value_t *root;
    json_result_t r = json_parse(pool, text, strlen(text), &root);
    CHECK(r == JSON_OK, "parses without error");
    return r == JSON_OK ? root : NULL;
}

static void test_scalars(void) {
    json_pool_t pool;
    json_pool_init(&pool);
    json_value_t *root = parse_ok(&pool, "  42.5  ");
    if (root) {
        CHECK(root->type == JSON_NUMBER, "bare number parses as JSON_NUMBER");
        CHECK(root->number == 42.5, "number value is correct");
    }
    json_pool_free(&pool);

    json_pool_init(&pool);
    root = parse_ok(&pool, "-3");
    if (root) CHECK(root->type == JSON_NUMBER && root->number == -3.0, "negative integer parses correctly");
    json_pool_free(&pool);

    json_pool_init(&pool);
    root = parse_ok(&pool, "true");
    if (root) CHECK(root->type == JSON_TRUE, "'true' parses as JSON_TRUE");
    json_pool_free(&pool);

    json_pool_init(&pool);
    root = parse_ok(&pool, "null");
    if (root) CHECK(root->type == JSON_NULL, "'null' parses as JSON_NULL");
    json_pool_free(&pool);
}

static void test_string_escapes(void) {
    json_pool_t pool;
    json_pool_init(&pool);
    /* zero-copy fast path: no escapes */
    json_value_t *root = parse_ok(&pool, "\"minecraft:block/stone\"");
    if (root) {
        CHECK(root->type == JSON_STRING, "plain string parses as JSON_STRING");
        CHECK(root->string.len == strlen("minecraft:block/stone") &&
              memcmp(root->string.ptr, "minecraft:block/stone", root->string.len) == 0,
              "plain string value is exact");
    }
    json_pool_free(&pool);

    json_pool_init(&pool);
    root = parse_ok(&pool, "\"line1\\nline2\\t\\\"quoted\\\"\"");
    if (root) {
        const char *expect = "line1\nline2\t\"quoted\"";
        CHECK(root->string.len == strlen(expect) && memcmp(root->string.ptr, expect, root->string.len) == 0,
              "escaped string decodes \\n \\t \\\" correctly");
    }
    json_pool_free(&pool);

    json_pool_init(&pool);
    root = parse_ok(&pool, "\"\\u00e9\""); /* e-acute, U+00E9 -> UTF-8 C3 A9 */
    if (root) {
        CHECK(root->string.len == 2 && (uint8_t)root->string.ptr[0] == 0xC3 && (uint8_t)root->string.ptr[1] == 0xA9,
              "\\uXXXX escape decodes to correct UTF-8 bytes");
    }
    json_pool_free(&pool);
}

static void test_nested_object_and_array(void) {
    json_pool_t pool;
    json_pool_init(&pool);
    json_value_t *root = parse_ok(&pool,
        "{\"a\": [1, 2, 3], \"b\": {\"c\": true, \"d\": null}, \"e\": []}");
    if (root) {
        CHECK(root->type == JSON_OBJECT, "root is an object");
        CHECK(json_object_count(root) == 3, "root has 3 keys");

        json_value_t *a = json_object_getz(&pool, root, "a");
        CHECK(a && a->type == JSON_ARRAY && json_array_count(a) == 3, "'a' is a 3-element array");
        if (a) {
            CHECK(json_array_at(&pool, a, 0)->number == 1, "a[0] == 1");
            CHECK(json_array_at(&pool, a, 2)->number == 3, "a[2] == 3");
        }

        json_value_t *b = json_object_getz(&pool, root, "b");
        CHECK(b && b->type == JSON_OBJECT, "'b' is an object");
        if (b) {
            json_value_t *c = json_object_getz(&pool, b, "c");
            CHECK(c && c->type == JSON_TRUE, "b.c is true");
            json_value_t *d = json_object_getz(&pool, b, "d");
            CHECK(d && d->type == JSON_NULL, "b.d is null");
        }

        json_value_t *e = json_object_getz(&pool, root, "e");
        CHECK(e && e->type == JSON_ARRAY && json_array_count(e) == 0, "'e' is an empty array");

        CHECK(json_object_getz(&pool, root, "missing") == NULL, "missing key returns NULL");
    }
    json_pool_free(&pool);
}

static void test_dynamic_keys(void) {
    /* Variant keys are arbitrary property strings
     * ("facing=east,half=bottom,shape=inner_left"), so callers iterate. */
    json_pool_t pool;
    json_pool_init(&pool);
    json_value_t *root = parse_ok(&pool,
        "{\"axis=x\": {\"model\": \"a\"}, \"axis=y\": {\"model\": \"b\"}}");
    if (root) {
        CHECK(json_object_count(root) == 2, "object has 2 dynamic keys");
        uint32_t len0;
        const char *key0 = json_object_key_at(&pool, root, 0, &len0);
        CHECK(len0 == strlen("axis=x") && memcmp(key0, "axis=x", len0) == 0, "key 0 is 'axis=x'");
        json_value_t *val0 = json_object_value_at(&pool, root, 0);
        json_value_t *model0 = json_object_getz(&pool, val0, "model");
        CHECK(model0 && model0->type == JSON_STRING && model0->string.len == 1 && model0->string.ptr[0] == 'a',
              "value at key 0 has model 'a'");
    }
    json_pool_free(&pool);
}

/* A real blockstates file shape (assets/minecraft/blockstates/oak_log.json). */
static void test_real_blockstate_shape(void) {
    static const char *text =
        "{\n"
        "  \"variants\": {\n"
        "    \"axis=x\": { \"model\": \"minecraft:block/oak_log_horizontal\", \"x\": 90, \"y\": 90 },\n"
        "    \"axis=y\": { \"model\": \"minecraft:block/oak_log\" },\n"
        "    \"axis=z\": { \"model\": \"minecraft:block/oak_log_horizontal\", \"x\": 90 }\n"
        "  }\n"
        "}\n";
    json_pool_t pool;
    json_pool_init(&pool);
    json_value_t *root = parse_ok(&pool, text);
    if (root) {
        json_value_t *variants = json_object_getz(&pool, root, "variants");
        CHECK(variants && variants->type == JSON_OBJECT && json_object_count(variants) == 3,
              "'variants' has 3 entries");
        json_value_t *axis_y = json_object_getz(&pool, variants, "axis=y");
        CHECK(axis_y != NULL, "'axis=y' variant found");
        if (axis_y) {
            json_value_t *model = json_object_getz(&pool, axis_y, "model");
            static const char *expect = "minecraft:block/oak_log";
            CHECK(model && model->type == JSON_STRING && model->string.len == strlen(expect) &&
                  memcmp(model->string.ptr, expect, model->string.len) == 0,
                  "axis=y model is 'minecraft:block/oak_log'");
        }
        json_value_t *axis_x = json_object_getz(&pool, variants, "axis=x");
        if (axis_x) {
            json_value_t *y_rot = json_object_getz(&pool, axis_x, "y");
            CHECK(y_rot && y_rot->type == JSON_NUMBER && y_rot->number == 90.0, "axis=x y-rotation is 90");
        }
    }
    json_pool_free(&pool);
}

int main(void) {
    test_scalars();
    test_string_escapes();
    test_nested_object_and_array();
    test_dynamic_keys();
    test_real_blockstate_shape();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
