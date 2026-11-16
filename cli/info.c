/*
 * prism info: what a world's level.dat says, as JSON, for the browser
 * build's page (prism serve reads it with read_level directly).
 *
 *   prism info <world dir>
 *
 * {"name":..., "version":..., "spawn":[x, z]}: its name as the game shows
 * it, the game version that last saved it and its spawn (26.x keeps it in
 * spawn.pos, older versions in SpawnX and SpawnZ).
 */
#include "cli.h"
#include "nbt_io.h"

static void nbt_text(const nbt_tag_t *t, char *out, size_t len) {
    if (!t || t->type != NBT_STRING) return;
    size_t n = (size_t)t->string.len < len - 1 ? (size_t)t->string.len : len - 1;
    memcpy(out, t->string.data, n);
    out[n] = 0;
}

void read_level(const char *world, level_t *lv) {
    char path[4096];
    snprintf(path, sizeof path, "%s/level.dat", world);
    nbt_pool_t pool;
    nbt_tag_t *root, *data, *t;
    nbt_pool_init(&pool);
    if (nbt_io_load_file(path, &pool, &root) == NBT_OK && (data = nbt_compound_getz(&pool, root, "Data"))) {
        nbt_text(nbt_compound_getz(&pool, data, "LevelName"), lv->name, sizeof lv->name);
        if ((t = nbt_compound_getz(&pool, data, "Version"))) nbt_text(nbt_compound_getz(&pool, t, "Name"), lv->version, sizeof lv->version);
        if ((t = nbt_compound_getz(&pool, data, "spawn")) && (t = nbt_compound_getz(&pool, t, "pos")) &&
            t->type == NBT_INT_ARRAY && t->int_array.len == 3) {
            lv->spawn_x = t->int_array.data[0];
            lv->spawn_z = t->int_array.data[2];
        } else {
            if ((t = nbt_compound_getz(&pool, data, "SpawnX")) && t->type == NBT_INT) lv->spawn_x = t->v_int;
            if ((t = nbt_compound_getz(&pool, data, "SpawnZ")) && t->type == NBT_INT) lv->spawn_z = t->v_int;
        }
    }
    nbt_pool_free(&pool);
}

/* s as a JSON string */
void json_str(str_t *o, const char *s) {
    sappend(o, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') sappend(o, "\\%c", c);
        else if (c < 0x20) sappend(o, "\\u%04x", c);
        else sappend(o, "%c", c);
    }
    sappend(o, "\"");
}

int cmd_info(int argc, char **argv) {
    if (argc != 2) { fputs("usage: prism info <world dir>\n", stderr); return 2; }
    level_t lv = { 0 };
    read_level(argv[1], &lv);
    str_t o = { 0 };
    sappend(&o, "{\"name\":");
    json_str(&o, lv.name);
    sappend(&o, ",\"version\":");
    json_str(&o, lv.version);
    sappend(&o, ",\"spawn\":[%d,%d]}\n", lv.spawn_x, lv.spawn_z);
    fwrite(o.p, 1, o.n, stdout);
    free(o.p);
    return 0;
}
