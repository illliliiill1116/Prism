/*
 * skin.h - the skin a player head's block entity names, shared by prism
 * and skinlist. Skins are fetched by fetch_skins.sh into <dir>/<hash>.png;
 * the bake itself never touches the network.
 */
#ifndef PRISM_SKIN_H
#define PRISM_SKIN_H

#include <string.h>

#include "nbt.h"

#define SKIN_HASH_MAX 80
#define SKIN_URL_PREFIX "http://textures.minecraft.net/texture/"

static inline int skin_b64(char c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 :
           c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
}

/* The skin hash of a head's block entity (a "minecraft:skull" with a
 * profile whose textures property points at textures.minecraft.net), as
 * lower-case hex; 0 if it has none. */
static inline int skin_hash_of(const nbt_pool_t *p, const nbt_tag_t *be, char hash[SKIN_HASH_MAX + 1]) {
    const nbt_tag_t *pr = nbt_compound_getz(p, be, "profile");
    const nbt_tag_t *props = pr && pr->type == NBT_COMPOUND ? nbt_compound_getz(p, pr, "properties") : NULL;
    for (uint32_t j = 0; props && props->type == NBT_LIST && j < props->child_count; j++) {
        const nbt_tag_t *prop = nbt_child_at(p, props, j);
        const nbt_tag_t *nm = nbt_compound_getz(p, prop, "name"), *v = nbt_compound_getz(p, prop, "value");
        if (!nm || nm->type != NBT_STRING || nm->string.len != 8 || memcmp(nm->string.data, "textures", 8)) continue;
        if (!v || v->type != NBT_STRING) continue;
        /* base64 -> JSON; only the url matters */
        char json[4096];
        size_t n = 0;
        unsigned acc = 0;
        int bits = 0;
        for (int32_t i = 0; i < v->string.len && n < sizeof json - 1; i++) {
            int d = skin_b64(v->string.data[i]);
            if (d < 0) continue;
            acc = acc << 6 | (unsigned)d;
            bits += 6;
            if (bits >= 8) { bits -= 8; json[n++] = (char)(acc >> bits & 255); }
        }
        json[n] = 0;
        const char *u = strstr(json, SKIN_URL_PREFIX);
        if (!u) return 0;
        u += sizeof SKIN_URL_PREFIX - 1;
        size_t h = 0;
        while (h < SKIN_HASH_MAX && ((u[h] >= '0' && u[h] <= '9') || (u[h] >= 'a' && u[h] <= 'f'))) { hash[h] = u[h]; h++; }
        hash[h] = 0;
        return h >= 32 && u[h] == '"';
    }
    return 0;
}

#endif
