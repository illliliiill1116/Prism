/* Block entity textures: banners with their patterns, player heads with
 * their skins and decorated pots with their sherds, each a texture of its
 * own for the block it sits on. */
#include "prism.h"

betex_t *betex;

uint32_t nbetex, cbetex;

bepos_t *bepos;

uint32_t bepos_cap = 1u << 12, nbepos;

const char *skin_dir;

int reg_x0, reg_z0; /* world x/z of grid cell 0 (set in main) */

long be_banners, be_heads, be_missing_skins, be_pots, be_moving;

static uint32_t bepos_slot(uint32_t gi) {
    uint32_t h = (gi * 2654435761u) & (bepos_cap - 1);
    while (bepos[h].gi && bepos[h].gi != gi + 1) h = (h + 1) & (bepos_cap - 1);
    return h;
}

const canvas_t *be_texture(int gx, int gy, int gz) {
    const bepos_t *b = &bepos[bepos_slot((uint32_t)gidx(gx, gy, gz))];
    return b->gi ? &betex[b->tex].tex : NULL;
}

static void be_put(int gx, int gy, int gz, const char *key, canvas_t (*make)(const void *), const void *arg) {
    uint32_t t = 0;
    while (t < nbetex && strcmp(betex[t].key, key)) t++;
    if (t == nbetex) {
        canvas_t c = make(arg);
        if (!c.pixels) return;
        if (nbetex == cbetex) {
            cbetex = cbetex ? cbetex * 2 : 64;
            betex = realloc(betex, cbetex * sizeof *betex);
            if (!betex) { fprintf(stderr, "oom\n"); exit(1); }
        }
        betex[nbetex].key = strdup(key);
        betex[nbetex].tex = c;
        nbetex++;
    }
    if (2 * (nbepos + 1) > bepos_cap) {
        bepos_t *old = bepos;
        uint32_t oc = bepos_cap;
        bepos_cap *= 2;
        bepos = calloc(bepos_cap, sizeof *bepos);
        for (uint32_t i = 0; i < oc; i++) if (old[i].gi) bepos[bepos_slot(old[i].gi - 1)] = old[i];
        free(old);
    }
    uint32_t gi = (uint32_t)gidx(gx, gy, gz), h = bepos_slot(gi);
    if (!bepos[h].gi) nbepos++;
    bepos[h] = (bepos_t){ gi + 1, t };
}

/* banner_pattern/<id>.json's asset_id: the texture under entity/banner/ */
static int banner_asset(const char *id, size_t n, char *out, size_t cap) {
    const char *c = memchr(id, ':', n);
    const char *ns = c ? id : "minecraft";
    size_t nsl = c ? (size_t)(c - id) : 9;
    const char *path = c ? c + 1 : id;
    size_t pl = c ? n - nsl - 1 : n;
    char zp[256];
    snprintf(zp, sizeof zp, "data/%.*s/banner_pattern/%.*s.json", (int)nsl, ns, (int)pl, path);
    snprintf(out, cap, "%.*s:entity/banner/%.*s", (int)nsl, ns, (int)pl, path);
    const zip_entry_t *e = zip_findz(&jarzip, zp);
    uint8_t *buf = NULL; size_t len = 0;
    if (e && zip_extract(&jarzip, e, &buf, &len) == ZIP_OK) {
        json_pool_t pool; json_pool_init(&pool);
        json_value_t *root, *a;
        if (json_parse(&pool, (const char *)buf, len, &root) == JSON_OK &&
            (a = json_object_getz(&pool, root, "asset_id")) && a->type == JSON_STRING) {
            const char *ac = memchr(a->string.ptr, ':', a->string.len);
            if (ac) snprintf(out, cap, "%.*s:entity/banner/%.*s", (int)(ac - a->string.ptr), a->string.ptr,
                             (int)(a->string.len - (size_t)(ac - a->string.ptr) - 1), ac + 1);
        }
        json_pool_free(&pool);
        free(buf);
    }
    return 1;
}

/* key: "<base colour>;<pattern asset>=<colour>;..." */
static canvas_t make_banner(const void *arg) {
    const char *key = arg;
    const canvas_t *base = blockmodel_texture(reg, "minecraft:entity/banner/banner_base", 35);
    if (!base) return (canvas_t){ 0 };
    canvas_t c = canvas_create(base->width, base->height);
    for (uint32_t y = 0; y < base->height; y++) memcpy(canvas_row(&c, y), canvas_row(base, y), (size_t)base->width * 4);
    /* the base layer, then each pattern: the mask's texels times the dye */
    char layer[256];
    const char *p = key;
    for (int first = 1; *p; first = 0) {
        const char *end = strchr(p, ';');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        const char *eq = first ? NULL : memchr(p, '=', n);
        uint8_t dye[3];
        int ok;
        if (first) {
            ok = blockmodel_dye_color(p, n, dye);
            snprintf(layer, sizeof layer, "minecraft:entity/banner/base");
        } else {
            ok = eq && blockmodel_dye_color(eq + 1, n - (size_t)(eq - p) - 1, dye);
            if (ok) snprintf(layer, sizeof layer, "%.*s", (int)(eq - p), p);
        }
        const canvas_t *m = ok ? blockmodel_texture(reg, layer, strlen(layer)) : NULL;
        if (m && m->width == c.width)
            for (uint32_t y = 0; y < c.height && y < m->height; y++) {
                const uint8_t *src = canvas_row(m, y);
                uint8_t *dst = canvas_row(&c, y);
                for (uint32_t x = 0; x < c.width; x++) {
                    unsigned a = src[x * 4 + 3];
                    if (!a) continue;
                    for (int k = 0; k < 3; k++) {
                        unsigned v = src[x * 4 + k] * dye[k] / 255;
                        dst[x * 4 + k] = (uint8_t)((v * a + dst[x * 4 + k] * (255 - a)) / 255);
                    }
                }
            }
        p += n;
        if (*p == ';') p++;
    }
    return c;
}

/* A skin as the game prepares it for a head: an old 64x32 skin widened
 * to 64x64; the head's own layer opaque; a hat layer with no clear texel
 * at all (an old skin's filled-in black) cleared; and the hat cut out at
 * half alpha, which keeps the head in the cut-out material. */
static canvas_t make_skin(const void *arg) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s.png", skin_dir, (const char *)arg);
    canvas_t png;
    if (png_decode_file(path, &png) != PNG_OK) { be_missing_skins++; return (canvas_t){ 0 }; }
    if (png.width != 64 || (png.height != 64 && png.height != 32)) { canvas_free(&png); return (canvas_t){ 0 }; }
    canvas_t c = canvas_create(64, 64);
    memset(c.pixels, 0, (size_t)64 * 64 * 4);
    for (uint32_t y = 0; y < png.height; y++) memcpy(canvas_row(&c, y), canvas_row(&png, y), 64 * 4);
    int legacy = png.height == 32;
    canvas_free(&png);
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 32; x++) canvas_row(&c, y)[x * 4 + 3] = 255;
    if (legacy) {
        int clear = 0;
        for (uint32_t y = 0; y < 32 && !clear; y++)
            for (uint32_t x = 32; x < 64; x++) if (canvas_row(&c, y)[x * 4 + 3] < 128) { clear = 1; break; }
        if (!clear)
            for (uint32_t y = 0; y < 32; y++) memset(canvas_row(&c, y) + 32 * 4, 0, 32 * 4);
    }
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) {
            uint8_t *px = canvas_row(&c, y) + x * 4;
            px[3] = px[3] >= 128 ? 255 : 0;
        }
    return c;
}

/* key: "pot:" and the back, left, right and front sides' pattern textures,
 * "-" for a plain one, ";" between */
static canvas_t make_pot(const void *arg) {
    char buf[1024], *sides[4] = { 0 };
    snprintf(buf, sizeof buf, "%s", (const char *)arg + 4);
    char *p = buf;
    for (int i = 0; i < 4 && p; i++) {
        char *end = strchr(p, ';');
        if (end) *end = 0;
        sides[i] = strcmp(p, "-") ? p : NULL;
        p = end ? end + 1 : NULL;
    }
    return blockmodel_pot_texture(reg, (const char *const *)sides);
}

static int nbt_streq(const nbt_tag_t *t, const char *s) {
    return t && t->type == NBT_STRING && (size_t)t->string.len == strlen(s) && memcmp(t->string.data, s, (size_t)t->string.len) == 0;
}

void load_block_entities(const nbt_pool_t *pool, const nbt_tag_t *root) {
    const nbt_tag_t *list = nbt_compound_getz(pool, root, "block_entities");
    for (uint32_t i = 0; list && i < list->child_count; i++) {
        const nbt_tag_t *be = nbt_child_at(pool, list, i);
        const nbt_tag_t *id = nbt_compound_getz(pool, be, "id");
        const nbt_tag_t *X = nbt_compound_getz(pool, be, "x"), *Y = nbt_compound_getz(pool, be, "y"), *Z = nbt_compound_getz(pool, be, "z");
        if (!id || !X || !Y || !Z || X->type != NBT_INT || Y->type != NBT_INT || Z->type != NBT_INT) continue;
        int gx = X->v_int - reg_x0, gy = Y->v_int - YMIN, gz = Z->v_int - reg_z0;
        if (gx < 0 || gz < 0 || gy < 0 || gx >= GW || gz >= GW || gy >= GH) continue;
        const state_t *st = &states[grid[gidx(gx, gy, gz)]];
        if (nbt_streq(id, "minecraft:piston")) {
            /* A block on the move (moving_piston): on loading, the game
             * finishes the move within a tick or two and the block it
             * carries settles here, so it is drawn settled. */
            const char *b = strchr(st->key, ':') ? strchr(st->key, ':') + 1 : st->key;
            uint32_t s;
            if (strncmp(b, "moving_piston", 13) == 0 && (b[13] == 0 || b[13] == '|') &&
                (s = intern_block_state(pool, nbt_compound_getz(pool, be, "blockState"))) != 0) {
                grid[gidx(gx, gy, gz)] = (uint16_t)s;
                be_moving++;
            }
            continue;
        }
        if (!st->be_tex) continue;
        char key[2048];
        if (nbt_streq(id, "minecraft:banner")) {
            /* the base colour is in the block's name */
            const char *nm = strchr(st->key, ':') ? strchr(st->key, ':') + 1 : st->key;
            const char *suf = strstr(nm, "_wall_banner");
            if (!suf) suf = strstr(nm, "_banner");
            if (!suf) continue;
            size_t kl = (size_t)snprintf(key, sizeof key, "%.*s", (int)(suf - nm), nm);
            const nbt_tag_t *pats = nbt_compound_getz(pool, be, "patterns");
            for (uint32_t j = 0; pats && j < pats->child_count && kl < sizeof key - 200; j++) {
                const nbt_tag_t *pt = nbt_child_at(pool, pats, j);
                const nbt_tag_t *pid = nbt_compound_getz(pool, pt, "pattern"), *col = nbt_compound_getz(pool, pt, "color");
                if (!col || col->type != NBT_STRING || !pid) continue;
                char asset[256];
                if (pid->type == NBT_STRING) banner_asset(pid->string.data, (size_t)pid->string.len, asset, sizeof asset);
                else if (pid->type == NBT_COMPOUND) {
                    /* an inline pattern names its texture itself */
                    const nbt_tag_t *a = nbt_compound_getz(pool, pid, "asset_id");
                    if (!a || a->type != NBT_STRING) continue;
                    const char *ac = memchr(a->string.data, ':', (size_t)a->string.len);
                    if (!ac) continue;
                    snprintf(asset, sizeof asset, "%.*s:entity/banner/%.*s", (int)(ac - a->string.data), a->string.data,
                             (int)(a->string.len - (ac - a->string.data) - 1), ac + 1);
                } else continue;
                kl += (size_t)snprintf(key + kl, sizeof key - kl, ";%s=%.*s", asset, (int)col->string.len, col->string.data);
            }
            be_put(gx, gy, gz, key, make_banner, key);
            be_banners++;
        } else if (nbt_streq(id, "minecraft:decorated_pot")) {
            /* sherds: back, left, right, front, as items; none when all four are bricks */
            const nbt_tag_t *sh = nbt_compound_getz(pool, be, "sherds");
            size_t kl = (size_t)snprintf(key, sizeof key, "pot:");
            int any = 0;
            for (uint32_t j = 0; j < 4; j++) {
                const nbt_tag_t *t = sh && j < sh->child_count ? nbt_child_at(pool, sh, j) : NULL;
                char pat[256];
                int has = t && t->type == NBT_STRING && blockmodel_pot_pattern(t->string.data, (size_t)t->string.len, pat, sizeof pat);
                kl += (size_t)snprintf(key + kl, sizeof key - kl, "%s%s", j ? ";" : "", has ? pat : "-");
                any |= has;
            }
            if (!any) continue;
            be_put(gx, gy, gz, key, make_pot, key);
            be_pots++;
        } else if (nbt_streq(id, "minecraft:skull") && skin_dir) {
            char hash[SKIN_HASH_MAX + 1];
            if (!skin_hash_of(pool, be, hash)) continue;
            snprintf(key, sizeof key, "skin:%s", hash);
            be_put(gx, gy, gz, key, make_skin, hash);
            be_heads++;
        }
    }
}
