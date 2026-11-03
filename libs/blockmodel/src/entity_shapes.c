/*
 * entity_shapes.c - geometry for block entities: chests, shulker boxes,
 * heads, banners, decorated pots, conduits and end portals and gateways;
 * no other block entity is drawn. Their models have no
 * "elements": the game draws them with entity renderers from textures
 * under textures/entity/. This synthesizes the same blockmodel_shape_t from
 * tables of cubes in block-local 16ths, authored facing north:
 *
 *   - cube_to_element(): Minecraft's box-UV unwrap - a cube at texture
 *     offset (tu,tv), size (w,h,d), maps its faces to fixed rectangles;
 *   - rotate_elem_y(): the facing rotation, N->E->S->W like shape.c.
 *
 * A banner's patterns, a player head's skin and a pot's sherds live in the
 * block entity, not the blockstate: here a banner is its plain dyed cloth,
 * a head is Steve and a pot has plain sides, and a caller that reads the
 * block entity swaps in the real texture, which keeps this layout.
 */
#include "internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Per-cube bits. */
enum {
    CUBE_DYED = 1u << 0, /* tinted with the block's dye colour (a banner's cloth) */
};

typedef struct {
    double x0, y0, z0;   /* block-local 16ths, y up, origin at cell corner */
    double w, h, d;      /* extents in 16ths (== texels) */
    int tu, tv;          /* texture atlas offset (top-left) in texels */
    unsigned flags;      /* CUBE_* gating bits (0 = always drawn) */
    double s;            /* 16ths per texel when the renderer scales the
                          * model (banner 2/3, a head's hat 17/16); 0 = 1 */
    double g;            /* grown by this much each way, its texture not
                          * (a CubeDeformation) */
} ent_cube_t;

typedef struct {
    const char *name;        /* block name after the "minecraft:" prefix */
    size_t name_len;
    const char *tex;         /* texture ref, e.g. "entity/chest/normal" (and
                              * its double chest's halves "..._left",
                              * "..._right") */
    int dbl;                 /* has the double chest halves */
} ent_def_t;

/* ---- box-UV unwrap ------------------------------------------------- */

static void set_face(blockmodel_shape_face_t *f, const canvas_t *tex,
                     double u0, double v0, double u1, double v1, int tw, int th) {
    f->texture = tex;
    /* shape_face uv is in 16ths of the texture (tile.c scales by the real
     * texture size): 16 units span the whole image on each axis. */
    f->u0 = (float)(u0 * 16.0 / tw); f->u1 = (float)(u1 * 16.0 / tw);
    f->v0 = (float)(v0 * 16.0 / th); f->v1 = (float)(v1 * 16.0 / th);
    f->uvrot = 0;
    f->tint = 0;
    f->has_tint_rgb = 0;
}

/* Entity box-UV layout (texels, v down), in ModelPart.Cube's slot order:
 *   row 0 (height d): [tu+d .. tu+d+w] DOWN, [tu+d+w .. tu+d+2w] UP
 *   row 1 (height h, at v+d): east(d) | north(w) | west(d) | south(w)
 * Side v runs top (y1) to bottom (y0), as in shape.c's face_uv_basis. */
/* Some entity renderers scale by a negative axis, which mirrors the model
 * and swaps opposite faces (shulker: scale(1,-1,-1); skull:
 * scale(-1,-1,1)). Geometry is authored in world space; only the swapped
 * face pairs' texture slots change. */
enum { FLIP_X = 1u << 0, /* swap east<->west  */
       FLIP_Y = 1u << 1, /* swap up<->down    */
       FLIP_Z = 1u << 2  /* swap north<->south */ };

static void cube_to_element(const ent_cube_t *c, const canvas_t *tex,
                            int tw, int th, unsigned flip, blockmodel_element_t *e) {
    double x0 = c->x0, y0 = c->y0, z0 = c->z0;
    double w = c->w, h = c->h, d = c->d, sc = c->s ? c->s : 1;
    double g = c->g;
    e->x0 = x0 - g; e->y0 = y0 - g; e->z0 = z0 - g;
    e->x1 = x0 + w * sc + g; e->y1 = y0 + h * sc + g; e->z1 = z0 + d * sc + g;
    e->rot_axis = 0; e->rot_angle = 0.0; e->rot_rescale = 0; e->has_xf = 0;
    e->rot_origin[0] = e->rot_origin[1] = e->rot_origin[2] = 8.0;
    memset(e->faces, 0, sizeof(e->faces));
    double tu = c->tu, tv = c->tv;
    int up    = (flip & FLIP_Y) ? FACE_DOWN  : FACE_UP;
    int down  = (flip & FLIP_Y) ? FACE_UP    : FACE_DOWN;
    int north = (flip & FLIP_Z) ? FACE_SOUTH : FACE_NORTH;
    int south = (flip & FLIP_Z) ? FACE_NORTH : FACE_SOUTH;
    int east  = (flip & FLIP_X) ? FACE_WEST  : FACE_EAST;
    int west  = (flip & FLIP_X) ? FACE_EAST  : FACE_WEST;
    /* up/down: u=+x (width w), v=+z (depth d) */
    set_face(&e->faces[down], tex, tu + d,      tv,     tu + d + w,   tv + d,     tw, th);
    set_face(&e->faces[up],   tex, tu + d + w,  tv,     tu + d + 2*w, tv + d,     tw, th);
    /* side strip at v+d, height h */
    set_face(&e->faces[east],  tex, tu,            tv + d, tu + d,         tv + d + h, tw, th);
    set_face(&e->faces[north], tex, tu + d,        tv + d, tu + d + w,     tv + d + h, tw, th);
    set_face(&e->faces[west],  tex, tu + d + w,    tv + d, tu + 2*d + w,   tv + d + h, tw, th);
    set_face(&e->faces[south], tex, tu + 2*d + w,  tv + d, tu + 2*d + 2*w, tv + d + h, tw, th);
}

/* ---- facing rotation ----------------------------------------------- */

/* Rotates an element k*90 degrees about the cell's vertical centre axis,
 * N->E->S->W per +90; faces move with it. */
static void rotate_elem_y(blockmodel_element_t *e, int k) {
    k &= 3;
    static const int cyc[4] = {FACE_NORTH, FACE_EAST, FACE_SOUTH, FACE_WEST};
    for (int s = 0; s < k; s++) {
        /* +90 about y through center 8: (x,z) -> (16-z, x) */
        double nx0 = 16.0 - e->z1, nx1 = 16.0 - e->z0;
        double nz0 = e->x0,        nz1 = e->x1;
        e->x0 = fmin(nx0, nx1); e->x1 = fmax(nx0, nx1);
        e->z0 = fmin(nz0, nz1); e->z1 = fmax(nz0, nz1);
        /* and its own turn: an axis along x comes to lie along +z, one
         * along z along -x */
        double ox = 16.0 - e->rot_origin[2];
        e->rot_origin[2] = e->rot_origin[0]; e->rot_origin[0] = ox;
        if (e->rot_axis == 0) e->rot_axis = 2;
        else if (e->rot_axis == 2) { e->rot_axis = 0; e->rot_angle = -e->rot_angle; }
        blockmodel_shape_face_t nf[FACE_COUNT];
        memset(nf, 0, sizeof(nf));
        nf[FACE_UP] = e->faces[FACE_UP];
        nf[FACE_DOWN] = e->faces[FACE_DOWN];
        for (int i = 0; i < 4; i++) nf[cyc[(i + 1) & 3]] = e->faces[cyc[i]];
        memcpy(e->faces, nf, sizeof(nf));
    }
}

/* facing -> k for rotate_elem_y (north 0, then N->E->S->W). Unknown -> 0. */
static int facing_k(const blockmodel_property_t *props, size_t prop_count) {
    for (size_t i = 0; i < prop_count; i++) {
        if (props[i].name_len == 6 && memcmp(props[i].name, "facing", 6) == 0) {
            const char *v = props[i].value; size_t n = props[i].value_len;
            if (n == 5 && memcmp(v, "north", 5) == 0) return 0;
            if (n == 4 && memcmp(v, "east", 4) == 0) return 1;
            if (n == 5 && memcmp(v, "south", 5) == 0) return 2;
            if (n == 4 && memcmp(v, "west", 4) == 0) return 3;
        }
    }
    return 0;
}

/* A floor skull's or standing banner's "rotation" (0..15, 22.5 deg each,
 * clockwise seen from above like N->E) as the nearest quarter turn, a k
 * for rotate_elem_y, plus the rest in *rest_deg (-45..22.5). Missing -> 0. */
static int rotation_k(const blockmodel_property_t *props, size_t prop_count, double *rest_deg) {
    *rest_deg = 0;
    for (size_t i = 0; i < prop_count; i++) {
        if (props[i].name_len == 8 && memcmp(props[i].name, "rotation", 8) == 0) {
            int r = 0;
            for (size_t j = 0; j < props[i].value_len; j++) {
                char c = props[i].value[j];
                if (c < '0' || c > '9') break;
                r = r * 10 + (c - '0');
            }
            int k = (r + 2) / 4;
            *rest_deg = (r - 4 * k) * 22.5;
            return k & 3;
        }
    }
    return 0;
}

/* The rest of a 22.5-degree rotation, clockwise seen from above like
 * rotate_elem_y, as a turn about the block's vertical centre line. */
static void spin_rest(blockmodel_element_t *e, uint32_t n, double rest_deg) {
    if (rest_deg == 0) return;
    double a = rest_deg * M_PI / 180.0, c = cos(a), s = sin(a);
    /* (x,z) -> (8 - (z-8)s + (x-8)c, 8 + (x-8)s + (z-8)c) */
    const double xf[3][4] = { { c, 0, -s, 8 - 8 * c + 8 * s }, { 0, 1, 0, 0 }, { s, 0, c, 8 - 8 * s - 8 * c } };
    for (uint32_t i = 0; i < n; i++) { e[i].has_xf = 1; memcpy(e[i].xf, xf, sizeof xf); }
}

/* A chest's "type": 0 single, 1 left, 2 right (the halves of a double
 * chest). */
static int chest_type(const blockmodel_property_t *props, size_t prop_count) {
    for (size_t i = 0; i < prop_count; i++) {
        if (props[i].name_len == 4 && memcmp(props[i].name, "type", 4) == 0) {
            const char *v = props[i].value; size_t n = props[i].value_len;
            return n == 4 && memcmp(v, "left", 4) == 0 ? 1 : n == 5 && memcmp(v, "right", 5) == 0 ? 2 : 0;
        }
    }
    return 0;
}

static int ends_with(const char *name, size_t nlen, const char *suf, size_t slen);

/* ---- block tables -------------------------------------------------- */

/* Closed chest (ChestModel, 64x64: base, lid and the lock on its front),
 * facing north: the front on the -z (north) side. A double chest is two
 * halves, each 15 wide, reaching into its partner's block, with half the
 * lock at that side: type=left's partner is the facing turned clockwise
 * (east, facing north), type=right's anticlockwise. */
static const ent_cube_t chest_cubes[3][3] = {
    { { 1, 0, 1, 14, 10, 14,  0, 19, 0, 0, 0 },   /* single: base */
      { 1, 9, 1, 14,  5, 14,  0,  0, 0, 0, 0 },   /* lid */
      { 7, 7, 0,  2,  4,  1,  0,  0, 0, 0, 0 } }, /* lock */
    { { 1, 0, 1, 15, 10, 14,  0, 19, 0, 0, 0 },   /* left */
      { 1, 9, 1, 15,  5, 14,  0,  0, 0, 0, 0 },
      { 15, 7, 0, 1,  4,  1,  0,  0, 0, 0, 0 } },
    { { 0, 0, 1, 15, 10, 14,  0, 19, 0, 0, 0 },   /* right */
      { 0, 9, 1, 15,  5, 14,  0,  0, 0, 0, 0 },
      { 0, 7, 0,  1,  4,  1,  0,  0, 0, 0, 0 } },
};

/* the waxed copper chests look the same as the unwaxed */
#define CHEST(name, tex, dbl) { name, sizeof name - 1, "entity/chest/" tex, dbl }
static const ent_def_t entity_defs[] = {
    CHEST("chest",                        "normal",           1),
    CHEST("trapped_chest",                "trapped",          1),
    CHEST("ender_chest",                  "ender",            0),
    CHEST("copper_chest",                 "copper",           1),
    CHEST("exposed_copper_chest",         "copper_exposed",   1),
    CHEST("weathered_copper_chest",       "copper_weathered", 1),
    CHEST("oxidized_copper_chest",        "copper_oxidized",  1),
    CHEST("waxed_copper_chest",           "copper",           1),
    CHEST("waxed_exposed_copper_chest",   "copper_exposed",   1),
    CHEST("waxed_weathered_copper_chest", "copper_weathered", 1),
    CHEST("waxed_oxidized_copper_chest",  "copper_oxidized",  1),
};
#undef CHEST

/* Closed shulker box (ShulkerModel, 64x64): base rim and lid dome. Always
 * upright - tilting onto a wall or ceiling isn't supported. */
static const ent_cube_t shulker_cubes[] = {
    { 0, 0, 0, 16,  8, 16,  0, 28, 0, 0, 0 }, /* base, texOffs(0,28) */
    { 0, 4, 0, 16, 12, 16,  0,  0, 0, 0, 0 }, /* lid,  texOffs(0,0)  */
};
#define SHULKER_CUBE_COUNT (uint32_t)(sizeof(shulker_cubes)/sizeof(shulker_cubes[0]))

/* Skulls and heads (SkullBlockRenderer), their cubes as the game's head
 * models lay them out: model space, y down, the face on -z, the origin at
 * the middle of the block's floor. The renderer turns that into the world
 * with scale(-1,-1,1): x' = 8 - x, y' = -y, z' = z + 8, so FLIP_X|FLIP_Y.
 * A cube can hang on a part turned about x or z through its pivot (the
 * dragon's jaw, the piglin's ears, at rest), and the whole head can be
 * moved down and scaled (the dragon's). A wall skull is raised and pushed
 * back against the wall, then turned by facing. */
typedef struct {
    double x, y, z; int w, h, d; /* the box, model space */
    int tu, tv;
    double g;                    /* CubeDeformation */
    int mirror;                  /* CubeListBuilder.mirror(): east and west
                                  * trade slots, every face flips in u */
    double px, py, pz;           /* the part's pivot */
    int axis; double rad;        /* the part's turn about x (0) or z (2) */
} head_cube_t;

static const head_cube_t skull_head[] = {
    { -4, -8, -4, 8, 8, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { -4, -8, -4, 8, 8, 8, 32, 0, 0.25, 0, 0, 0, 0, 0, 0 }, /* the hat of a humanoid's */
};
/* PiglinModel.addHead; PiglinHeadModel's ears hang at -(cos 0 + 2.5) * 0.2 */
static const head_cube_t piglin_head[] = {
    { -5, -8, -4, 10, 8, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { -2, -4, -5, 4, 4, 1, 31, 1, 0, 0, 0, 0, 0, 0, 0 },     /* snout */
    { 2, -2, -5, 1, 2, 1, 2, 4, 0, 0, 0, 0, 0, 0, 0 },       /* tusks */
    { -3, -2, -5, 1, 2, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, -2, 1, 5, 4, 51, 6, 0, 0, 4.5, -6, 0, 2, -0.7 }, /* left ear */
    { -1, 0, -2, 1, 5, 4, 39, 6, 0, 0, -4.5, -6, 0, 2, 0.7 }, /* right ear */
};
/* DragonHeadModel; its jaw hangs open at (sin 0 + 1) * 0.2 */
static const head_cube_t dragon_head[] = {
    { -6, -1, -24, 12, 5, 16, 176, 44, 0, 0, 0, 0, 0, 0, 0 },  /* upper lip */
    { -8, -8, -10, 16, 16, 16, 112, 30, 0, 0, 0, 0, 0, 0, 0 }, /* upper head */
    { -5, -12, -4, 2, 4, 6, 0, 0, 0, 1, 0, 0, 0, 0, 0 },       /* scales */
    { 3, -12, -4, 2, 4, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { -5, -3, -22, 2, 2, 4, 112, 0, 0, 1, 0, 0, 0, 0, 0 },     /* nostrils */
    { 3, -3, -22, 2, 2, 4, 112, 0, 0, 0, 0, 0, 0, 0, 0 },
    { -6, 0, -16, 12, 4, 16, 176, 65, 0, 0, 0, 4, -8, 0, 0.2 }, /* jaw */
};

typedef struct {
    const char *mob; size_t mob_len; const char *tex; int tw, th;
    const head_cube_t *cubes; uint32_t n;
    double oy, s; /* the head part's offset down (y) and scale */
} skull_def_t;
#define SK(m, t, w, h, cubes, n, oy, s) { m, sizeof(m) - 1, t, w, h, cubes, n, oy, s }
static const skull_def_t skull_defs[] = {
    SK("skeleton",        "entity/skeleton/skeleton",        64, 32, skull_head, 1, 0, 1),
    SK("wither_skeleton", "entity/skeleton/wither_skeleton", 64, 32, skull_head, 1, 0, 1),
    SK("zombie",          "entity/zombie/zombie",            64, 64, skull_head, 2, 0, 1),
    SK("creeper",         "entity/creeper/creeper",          64, 32, skull_head, 1, 0, 1),
    SK("player",          "entity/player/wide/steve",        64, 64, skull_head, 2, 0, 1),
    SK("piglin",          "entity/piglin/piglin",            64, 64, piglin_head, 6, 0, 1),
    /* offset(0, -7.986666, 0).scaled(0.75), which scales the offset too */
    SK("dragon",          "entity/enderdragon/dragon",       256, 256, dragon_head, 7, -7.986666 * 0.75, 0.75),
};

/* A head's cubes as elements, unturned (see above). */
static uint32_t head_elements(const skull_def_t *sd, const canvas_t *tex, int wall,
                              blockmodel_element_t *accum, uint32_t cap) {
    double s = sd->s, wy = wall ? 4 : 0, wz = wall ? 4 : 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < sd->n && n < cap; i++) {
        const head_cube_t *c = &sd->cubes[i];
        double mx1 = s * (c->px + c->x + c->w), my1 = sd->oy + s * (c->py + c->y + c->h);
        double mz0 = s * (c->pz + c->z);
        ent_cube_t ec = { 8 - mx1, -my1 + wy, mz0 + 8 + wz, c->w, c->h, c->d, c->tu, c->tv, 0, s, c->g * s };
        blockmodel_element_t *e = &accum[n++];
        cube_to_element(&ec, tex, sd->tw, sd->th, FLIP_X | FLIP_Y, e);
        if (c->mirror) {
            blockmodel_shape_face_t t = e->faces[FACE_EAST];
            e->faces[FACE_EAST] = e->faces[FACE_WEST];
            e->faces[FACE_WEST] = t;
            for (int f = 0; f < FACE_COUNT; f++) {
                float u = e->faces[f].u0; e->faces[f].u0 = e->faces[f].u1; e->faces[f].u1 = u;
            }
        }
        if (c->rad != 0) {
            /* scale(-1,-1,1) turns a turn about x the other way, about z not */
            e->rot_axis = c->axis;
            e->rot_angle = (c->axis == 0 ? -c->rad : c->rad) * 180 / M_PI;
            e->rot_origin[0] = 8 - s * c->px;
            e->rot_origin[1] = -(sd->oy + s * c->py) + wy;
            e->rot_origin[2] = s * c->pz + 8 + wz;
        }
    }
    return n;
}

/* Banner (BannerModel, 64x64, drawn at scale(2/3,-2/3,-2/3), so
 * FLIP_Y|FLIP_Z): the cloth texOffs(0,0) 20x40x1, the bar texOffs(0,42)
 * 20x2x2 and, standing, the pole texOffs(44,0) 2x42x2. Laid out for a
 * standing banner at rotation 0 and a wall banner facing south: the
 * cloth's front, with the pattern, faces south. The cloth's sway is left
 * out. A wall banner hangs from near the top of its block down into the
 * one below. */
#define B (2.0 / 3)
static const ent_cube_t banner_standing_cubes[] = {
    { 4.0 / 3,  8.0 / 3, 26.0 / 3, 20, 40, 1,  0,  0, CUBE_DYED, B, 0 }, /* cloth */
    { 22.0 / 3,       0, 22.0 / 3,  2, 42, 2, 44,  0, 0, B, 0 },         /* pole */
    { 4.0 / 3,       28, 22.0 / 3, 20,  2, 2,  0, 42, 0, B, 0 },         /* bar */
};
static const ent_cube_t banner_wall_cubes[] = {
    { 4.0 / 3,      -13,  5.0 / 3, 20, 40, 1,  0,  0, CUBE_DYED, B, 0 }, /* cloth */
    { 4.0 / 3, 37.0 / 3,  1.0 / 3, 20,  2, 2,  0, 42, 0, B, 0 },         /* bar */
};
#undef B

/* Decorated pot (DecoratedPotRenderer), in one texture of POT_W x POT_H
 * (blockmodel_pot_texture): the base layer's 32 x 32 at 0,0 - the neck,
 * two boxes turned upside down (xRot 180: FLIP_Y|FLIP_Z), the rim 8x3x8
 * shrunk by 0.1 over a 6x1x6 grown by 0.2, and the flat 14x14 top and
 * bottom - and the sides layer's 16 x 16 per side at pot_side_at: a
 * 14x16 plane on each side. Laid out facing north, whose front is on the
 * south side (the renderer turns by 180 - yRot). */
#define POT_W 64
#define POT_H 32
static const ent_cube_t pot_neck_cubes[] = {
    { 4, 17, 4, 8, 3, 8, 0, 0, 0, 0, -0.1 }, /* rim */
    { 5, 16, 5, 6, 1, 6, 0, 5, 0, 0, 0.2 },
};
static const int pot_side_at[4][2] = { { 32, 0 }, { 48, 0 }, { 32, 16 }, { 48, 16 } }; /* back, left, right, front */

canvas_t blockmodel_pot_texture(blockmodel_registry_t *reg, const char *const sides[4]) {
    const canvas_t *base = bm_load_texture_cached(reg, "minecraft:entity/decorated_pot/decorated_pot_base", 49);
    const canvas_t *plain = bm_load_texture_cached(reg, "minecraft:entity/decorated_pot/decorated_pot_side", 49);
    if (!base || !plain) return (canvas_t){ 0 };
    canvas_t c = canvas_create(POT_W, POT_H);
    if (!c.pixels) return c;
    memset(c.pixels, 0, (size_t)POT_W * POT_H * 4);
    /* each part nearest-scaled into its place, as a resource pack's may
       be larger */
    for (int part = 0; part < 5; part++) {
        const canvas_t *t = part ? (sides && sides[part - 1] ? bm_load_texture_cached(reg, sides[part - 1], strlen(sides[part - 1])) : NULL) : base;
        if (part && !t) t = plain;
        int x0 = part ? pot_side_at[part - 1][0] : 0, y0 = part ? pot_side_at[part - 1][1] : 0, n = part ? 16 : 32;
        for (int y = 0; y < n; y++) {
            const uint8_t *src = canvas_row(t, (uint32_t)((size_t)y * t->height / n));
            uint8_t *dst = canvas_row(&c, (uint32_t)(y0 + y)) + x0 * 4;
            for (int x = 0; x < n; x++) memcpy(dst + x * 4, src + (size_t)x * t->width / n * 4, 4);
        }
    }
    return c;
}

int blockmodel_pot_pattern(const char *item, size_t len, char *out, size_t cap) {
    const char *colon = memchr(item, ':', len);
    const char *ns = colon ? item : "minecraft";
    size_t nsl = colon ? (size_t)(colon - item) : 9, nl = colon ? len - nsl - 1 : len;
    const char *nm = colon ? colon + 1 : item;
    if (!ends_with(nm, nl, "_pottery_sherd", 14)) return 0;
    snprintf(out, cap, "%.*s:entity/decorated_pot/%.*s_pottery_pattern", (int)nsl, ns, (int)(nl - 14), nm);
    return 1;
}

/* ---- copper golem statue ------------------------------------------- */

/* Models posed by nested parts, as the game builds them: each part hangs
 * on its parent at an offset, turned by rotationZYX(z, y, x); cubes sit
 * in their part's frame (model space, y down). Each cube becomes an
 * element whose xf carries it to the world. */
typedef struct { int parent; double x, y, z, xr, yr, zr; } ent_part_t;
typedef struct { int part; double x, y, z, w, h, d; int tu, tv; double g; } ent_pcube_t;

/* CopperGolemModel's body layers, one per pose (generated from the jar's
 * createBodyLayer, createRunningPoseBodyLayer, createSittingPoseBodyLayer
 * and createStarPoseBodyLayer). The eyes' glowing layer is left out. */
static const ent_part_t golem_standing_parts[] = {
    { -1, 0, -5, 0, 0, 0, 0 }, /* body */
    { 0, 0, -6, 0, 0, 0, 0 }, /* head */
    { 0, -4, -6, 0, 0, 0, 0 }, /* right_arm */
    { 0, 4, -6, 0, 0, 0, 0 }, /* left_arm */
    { -1, 0, -5, 0, 0, 0, 0 }, /* right_leg */
    { -1, 0, -5, 0, 0, 0, 0 }, /* left_leg */
};
static const ent_pcube_t golem_standing_cubes[] = {
    { 0, -4, -6, -3, 8, 6, 6, 0, 15, 0 },
    { 1, -4, -5, -5, 8, 5, 10, 0, 0, 0.015 },
    { 1, -1, -2, -6, 2, 3, 2, 56, 0, 0 },
    { 1, -1, -9, -1, 2, 4, 2, 37, 8, -0.015 },
    { 1, -2, -13, -2, 4, 4, 4, 37, 0, -0.015 },
    { 2, -3, -1, -2, 3, 10, 4, 36, 16, 0 },
    { 3, 0, -1, -2, 3, 10, 4, 50, 16, 0 },
    { 4, -4, 0, -2, 4, 5, 4, 0, 27, 0 },
    { 5, 0, 0, -2, 4, 5, 4, 16, 27, 0 },
};
static const ent_part_t golem_running_parts[] = {
    { -1, -1.064, -5, 0, 0, 0, 0 }, /* body */
    { 0, 1.1, 0.1, 0.7, 0.1204, -0.0064, -0.0779 }, /* body_r1 */
    { 0, 0.7, -5.6, -1.8, 0, 0, 0 }, /* head */
    { 0, -4, -6, 0, 0, 0, 0 }, /* right_arm */
    { 3, 0.7, -0.248, -1.62, 1.0036, 0, 0 }, /* right_arm_r1 */
    { 0, 4, -6, 0, 0, 0, 0 }, /* left_arm */
    { 5, 0.732, 0, 0, -0.8715, -0.0535, -0.0449 }, /* left_arm_r1 */
    { -1, -3.064, -5, 0, 0, 0, 0 }, /* right_leg */
    { 7, 1.048, 0, -0.9, -0.8727, 0, 0 }, /* right_leg_r1 */
    { -1, 0.936, -5, 0, 0, 0, 0 }, /* left_leg */
    { 9, 1, 0, 0, 0.7854, 0, 0 }, /* left_leg_r1 */
};
static const ent_pcube_t golem_running_cubes[] = {
    { 1, -4.02, -6.116, -3.5, 8, 6, 6, 0, 15, 0 },
    { 2, -4, -5.1, -5, 8, 5, 10, 0, 0, 0 },
    { 2, -1.02, -2.1, -6, 2, 3, 2, 56, 0, 0 },
    { 2, -1.02, -9.1, -1, 2, 4, 2, 37, 8, -0.015 },
    { 2, -2, -13.1, -2, 4, 4, 4, 37, 0, -0.015 },
    { 4, -3.052, -1.11, -2.036, 3, 10, 4, 36, 16, 0 },
    { 6, 0.032, -1.1, -2, 3, 10, 4, 50, 16, 0 },
    { 8, -1.856, -0.1, -1.09, 4, 5, 4, 0, 27, 0 },
    { 10, -2.088, -0.1, -2, 4, 5, 4, 16, 27, 0 },
};
static const ent_part_t golem_sitting_parts[] = {
    { -1, 0, -3, 2.325, 0, 0, 0 }, /* body */
    { 0, 0, -1, -4.325, 0, 0, -3.1416 }, /* body_r1 */
    { 0, 0, -6, -0.2, 0, 0, 0 }, /* head */
    { 0, -4, -5.6, -1.8, 0.4363, 0, 0 }, /* right_arm */
    { 3, 0, 0.0893, 0.1198, -1.0472, 0, 0 }, /* right_arm_r1 */
    { 0, 4, -5.6, -1.7, 0.4363, 0, 0 }, /* left_arm */
    { 5, 0, -0.0015, -0.0808, -1.0472, 0, 0 }, /* left_arm_r1 */
    { -1, -2.1, -2.1, -2.075, 0, 0, 0 }, /* right_leg */
    { 7, 0.05, -1.9, 1.075, -1.5708, 0, 0 }, /* right_leg_r1 */
    { -1, 2, -2, -2.075, 0, 0, 0 }, /* left_leg */
    { 9, 0.05, -2, 1.075, -1.5708, 0, 0 }, /* left_leg_r1 */
};
static const ent_pcube_t golem_sitting_cubes[] = {
    { 0, -3, -4, -4.525, 6, 1, 6, 3, 19, 0 },
    { 0, -4, -3, -3.525, 8, 6, 6, 0, 15, 0 },
    { 1, -4, -3, -2.2, 8, 6, 3, 3, 18, 0 },
    { 2, -1, -7, -3.3, 2, 4, 2, 37, 8, -0.015 },
    { 2, -2, -11, -4.3, 4, 4, 4, 37, 0, -0.015 },
    { 2, -4, -3, -7.325, 8, 5, 10, 0, 0, 0 },
    { 2, -1, 0, -8.325, 2, 3, 2, 56, 0, 0 },
    { 4, -3.075, -0.9733, -1.9966, 3, 10, 4, 36, 16, 0 },
    { 6, 0.075, -1.0443, -1.8997, 3, 10, 4, 50, 16, 0 },
    { 8, -2, 0.975, 0, 4, 5, 4, 0, 27, 0 },
    { 10, -2, 0.975, 0, 4, 5, 4, 16, 27, 0 },
};
static const ent_part_t golem_star_parts[] = {
    { -1, 0, -5, 0, 0, 0, 0 }, /* body */
    { 0, 0, -6, 0, 0, 0, 0 }, /* head */
    { 0, -4, -6, 0, 0, 0, 0 }, /* right_arm */
    { 2, 1, 1, 0, 0, 0, 1.9199 }, /* right_arm_r1 */
    { 2, -1, 7.4, -1, 0, 0, 0 }, /* rightItem */
    { 0, 4, -6, 0, 0, 0, 0 }, /* left_arm */
    { 5, -1, 1, 0, 0, 0, -1.9199 }, /* left_arm_r1 */
    { -1, -3, -5, 0, 0, 0, 0 }, /* right_leg */
    { 7, 0.35, 2, 0.01, 0, 0, 0.2618 }, /* right_leg_r1 */
    { -1, 1, -5, 0, 0, 0, 0 }, /* left_leg */
    { 9, 1.65, 2, 0, 0, 0, -0.2618 }, /* left_leg_r1 */
};
static const ent_pcube_t golem_star_cubes[] = {
    { 0, -4, -6, -3, 8, 6, 6, 0, 15, 0 },
    { 1, -4, -5, -5, 8, 5, 10, 0, 0, 0 },
    { 1, -1, -2, -6, 2, 3, 2, 56, 0, 0 },
    { 1, -1, -9, -1, 2, 4, 2, 37, 8, -0.015 },
    { 1, -2, -13, -2, 4, 4, 4, 37, 0, -0.015 },
    { 3, -1.5, -5, -2, 3, 10, 4, 36, 16, 0 },
    { 6, -1.5, -5, -2, 3, 10, 4, 50, 16, 0 },
    { 8, -2, -2.5, -2, 4, 5, 4, 0, 27, 0 },
    { 10, -2, -2.5, -2, 4, 5, 4, 16, 27, 0 },
};

static const struct {
    const char *pose; const ent_part_t *parts; uint32_t np; const ent_pcube_t *cubes; uint32_t nc;
} golem_poses[] = {
#define GP(p, t) { p, t##_parts, sizeof t##_parts / sizeof t##_parts[0], t##_cubes, sizeof t##_cubes / sizeof t##_cubes[0] }
    GP("standing", golem_standing), GP("running", golem_running),
    GP("sitting", golem_sitting), GP("star", golem_star),
#undef GP
};

/* a = a * b, 3x4 affines (the last row 0 0 0 1) */
static void affine_mul(double a[3][4], const double b[3][4]) {
    double r[3][4];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + (j == 3 ? a[i][3] : 0);
    memcpy(a, r, sizeof r);
}

/* translate(x, y, z) * rotationZYX(zr, yr, xr) */
static void part_affine(double m[3][4], double x, double y, double z, double xr, double yr, double zr) {
    double cx = cos(xr), sx = sin(xr), cy = cos(yr), sy = sin(yr), cz = cos(zr), sz = sin(zr);
    const double r[3][4] = {
        { cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx, x },
        { sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx, y },
        { -sy, cy * sx, cy * cx, z },
    };
    memcpy(m, r, sizeof r);
}

/* The posed model's cubes as elements; `root` takes model space to the
 * block (16ths). */
static uint32_t posed_elements(const ent_part_t *parts, uint32_t np, const ent_pcube_t *cubes, uint32_t nc,
                               const double root[3][4], const canvas_t *tex, int tw, int th,
                               blockmodel_element_t *accum, uint32_t cap) {
    double m[32][3][4];
    if (np > 32) return 0;
    for (uint32_t i = 0; i < np; i++) {
        const ent_part_t *p = &parts[i];
        memcpy(m[i], p->parent < 0 ? root : m[p->parent], sizeof m[i]);
        double local[3][4];
        part_affine(local, p->x, p->y, p->z, p->xr, p->yr, p->zr);
        affine_mul(m[i], local);
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < nc && n < cap; i++) {
        const ent_pcube_t *c = &cubes[i];
        ent_cube_t ec = { c->x, c->y, c->z, c->w, c->h, c->d, c->tu, c->tv, 0, 0, c->g };
        blockmodel_element_t *e = &accum[n++];
        cube_to_element(&ec, tex, tw, th, 0u, e);
        e->has_xf = 1;
        memcpy(e->xf, m[c->part], sizeof e->xf);
    }
    return n;
}

/* ---- end portal / gateway ----------------------------------------- */

/* Their starfield (rendertype_end_portal.fsh) is drawn in screen space,
 * so it slides as the camera moves: here one still of it, at GameTime 0,
 * is pinned to each face, a face showing PORTAL_SPAN of the screen.
 * Layers of end_portal.png, each scaled, turned and tinted, over the
 * tinted end sky; the matrices are built column-major and applied to a
 * row vector, as in the shader. */
#define PORTAL_N 64
#define PORTAL_SPAN 0.125
static const float portal_colors[16][3] = {
    { 0.022087f, 0.098399f, 0.110818f }, { 0.011892f, 0.095924f, 0.089485f },
    { 0.027636f, 0.101689f, 0.100326f }, { 0.046564f, 0.109883f, 0.114838f },
    { 0.064901f, 0.117696f, 0.097189f }, { 0.063761f, 0.086895f, 0.123646f },
    { 0.084817f, 0.111994f, 0.166380f }, { 0.097489f, 0.154120f, 0.091064f },
    { 0.106152f, 0.131144f, 0.195191f }, { 0.097721f, 0.110188f, 0.187229f },
    { 0.133516f, 0.138278f, 0.148582f }, { 0.070006f, 0.243332f, 0.235792f },
    { 0.196766f, 0.142899f, 0.214696f }, { 0.047281f, 0.315338f, 0.321970f },
    { 0.204675f, 0.390010f, 0.302066f }, { 0.080955f, 0.314821f, 0.661491f },
};

typedef double mat4_t[4][4]; /* [column][row] */

static void mat4_mul(const mat4_t a, const mat4_t b, mat4_t out) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            double v = 0;
            for (int k = 0; k < 4; k++) v += a[k][r] * b[c][k];
            out[c][r] = v;
        }
}

/* texture(tex, p.xy / p.w), repeating, nearest */
static const uint8_t *portal_sample(const canvas_t *t, const double p[4]) {
    double u = p[0] / p[3], v = p[1] / p[3];
    u -= floor(u); v -= floor(v);
    uint32_t x = (uint32_t)(u * t->width) % t->width, y = (uint32_t)(v * t->height) % t->height;
    return canvas_row(t, y) + x * 4;
}

/* The gateway draws 16 layers, the portal 15. */
static canvas_t portal_texture(blockmodel_registry_t *reg, int layers) {
    const canvas_t *sky = bm_load_texture_cached(reg, "environment/end_sky", 19);
    const canvas_t *stars = bm_load_texture_cached(reg, "entity/end_portal/end_portal", 28);
    if (!sky || !stars || !sky->pixels || !stars->pixels) return (canvas_t){ 0 };
    canvas_t c = canvas_create(PORTAL_N, PORTAL_N);
    if (!c.pixels) return c;
    static const mat4_t st = { { 0.5, 0, 0, 0.25 }, { 0, 0.5, 0, 0.25 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    mat4_t m[16];
    for (int i = 0; i < layers; i++) {
        double l = i + 1, a = (l * l * 4321.0 + l * 9.0) * 2.0 * M_PI / 180.0, sc = (4.5 - l / 4.0) * 2.0;
        mat4_t t = { { 1, 0, 0, 17.0 / l }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
        mat4_t rs = { { sc * cos(a), -sc * sin(a), 0, 0 }, { sc * sin(a), sc * cos(a), 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
        mat4_t tst;
        mat4_mul(t, st, tst);
        mat4_mul(rs, tst, m[i]);
    }
    for (int y = 0; y < PORTAL_N; y++) {
        uint8_t *row = canvas_row(&c, (uint32_t)y);
        for (int x = 0; x < PORTAL_N; x++) {
            double v[4] = { (x + 0.5) / PORTAL_N * PORTAL_SPAN, (y + 0.5) / PORTAL_N * PORTAL_SPAN, 0, 1 };
            const uint8_t *k = portal_sample(sky, v);
            double col[3];
            for (int ch = 0; ch < 3; ch++) col[ch] = k[ch] / 255.0 * portal_colors[0][ch];
            for (int i = 0; i < layers; i++) {
                double t[4];
                for (int cc = 0; cc < 4; cc++)
                    t[cc] = v[0] * m[i][cc][0] + v[1] * m[i][cc][1] + v[2] * m[i][cc][2] + v[3] * m[i][cc][3];
                const uint8_t *q = portal_sample(stars, t);
                for (int ch = 0; ch < 3; ch++) col[ch] += q[ch] / 255.0 * portal_colors[i][ch];
            }
            for (int ch = 0; ch < 3; ch++) row[x * 4 + ch] = (uint8_t)(fmin(col[ch], 1.0) * 255.0 + 0.5);
            row[x * 4 + 3] = 255;
        }
    }
    return c;
}

/* A flat face of a pot: the element from x0,y0,z0 to x1,y1,z1 (one of
 * them flat) with only face f, its uv u0,v0..u1,v1 in POT texels. */
static void pot_plane(blockmodel_element_t *e, const canvas_t *tex, double x0, double y0, double z0,
                      double x1, double y1, double z1, int f, double u0, double v0, double u1, double v1) {
    memset(e, 0, sizeof *e);
    e->x0 = x0; e->y0 = y0; e->z0 = z0; e->x1 = x1; e->y1 = y1; e->z1 = z1;
    e->rot_origin[0] = e->rot_origin[1] = e->rot_origin[2] = 8.0;
    set_face(&e->faces[f], tex, u0, v0, u1, v1, POT_W, POT_H);
}

/* DyeColor's texture colours, which tint banner cloth and patterns. */
static const struct { const char *name; uint8_t rgb[3]; } dyes[] = {
    { "white", { 0xF9, 0xFF, 0xFE } },      { "orange", { 0xF9, 0x80, 0x1D } },
    { "magenta", { 0xC7, 0x4E, 0xBD } },    { "light_blue", { 0x3A, 0xB3, 0xDA } },
    { "yellow", { 0xFE, 0xD8, 0x3D } },     { "lime", { 0x80, 0xC7, 0x1F } },
    { "pink", { 0xF3, 0x8B, 0xAA } },       { "gray", { 0x47, 0x4F, 0x52 } },
    { "light_gray", { 0x9D, 0x9D, 0x97 } }, { "cyan", { 0x16, 0x9C, 0x9C } },
    { "purple", { 0x89, 0x32, 0xB8 } },     { "blue", { 0x3C, 0x44, 0xAA } },
    { "brown", { 0x83, 0x54, 0x32 } },      { "green", { 0x5E, 0x7C, 0x16 } },
    { "red", { 0xB0, 0x2E, 0x26 } },        { "black", { 0x1D, 0x1D, 0x21 } },
};

int blockmodel_dye_color(const char *name, size_t len, uint8_t rgb[3]) {
    if (len > 10 && memcmp(name, "minecraft:", 10) == 0) { name += 10; len -= 10; }
    for (size_t i = 0; i < sizeof dyes / sizeof dyes[0]; i++)
        if (strlen(dyes[i].name) == len && memcmp(dyes[i].name, name, len) == 0) {
            memcpy(rgb, dyes[i].rgb, 3);
            return 1;
        }
    return 0;
}

/* ---- dispatch ------------------------------------------------------ */

/* Emits a cube table into accum, rotated by `k`. Returns the count
 * written (<= cap). */
static uint32_t fill_cubes(const ent_cube_t *cubes, uint32_t count,
                           const canvas_t *tex, int tw, int th, unsigned flip,
                           int k, const uint8_t *dye,
                           blockmodel_element_t *accum, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < count && n < cap; i++) {
        const ent_cube_t *c = &cubes[i];
        blockmodel_element_t *e = &accum[n++];
        cube_to_element(c, tex, tw, th, flip, e);
        if ((c->flags & CUBE_DYED) && dye)
            for (int f = 0; f < FACE_COUNT; f++) {
                e->faces[f].has_tint_rgb = 1;
                memcpy(e->faces[f].tint_rgb, dye, 3);
            }
        if (k) rotate_elem_y(e, k);
    }
    return n;
}

/* Suffix match: does `name` (length nlen) end with the literal `suf`? */
static int ends_with(const char *name, size_t nlen, const char *suf, size_t slen) {
    return nlen >= slen && memcmp(name + nlen - slen, suf, slen) == 0;
}

int bm_entity_shape(blockmodel_registry_t *reg, const char *block_name, size_t name_len,
                    const blockmodel_property_t *props, size_t prop_count,
                    blockmodel_element_t *accum, uint32_t *accum_count, uint32_t cap,
                    int *any_rot) {
    /* strip a leading "minecraft:" (or any namespace) */
    const char *name = block_name;
    size_t nlen = name_len;
    const char *colon = (const char *)memchr(block_name, ':', name_len);
    if (colon) { name = colon + 1; nlen = name_len - (size_t)(colon - block_name) - 1; }

    /* ---- family: shulker box (17 colors) --------------------------- */
    /* "shulker_box" (default) or "<color>_shulker_box"; the color slots
     * straight into the texture name (entity/shulker/shulker[_<color>]). */
    if (ends_with(name, nlen, "shulker_box", 11)) {
        char ref[64];
        if (nlen == 11) {
            memcpy(ref, "entity/shulker/shulker", 23); /* incl NUL */
        } else if (name[nlen - 12] == '_') {
            int clen = (int)(nlen - 12); /* color prefix */
            snprintf(ref, sizeof ref, "entity/shulker/shulker_%.*s", clen, name);
        } else {
            return 0;
        }
        const canvas_t *tex = bm_load_texture_cached(reg, ref, strlen(ref));
        /* ShulkerBoxRenderer draws the model with scale(1,-1,-1) */
        *accum_count = fill_cubes(shulker_cubes, SHULKER_CUBE_COUNT, tex, 64, 64,
                                  FLIP_Y | FLIP_Z, 0, NULL, accum, cap);
        *any_rot = 0;
        return 1;
    }

    /* ---- family: skull / head -------------------------------------- */
    /* "<mob>_skull", "<mob>_wall_skull", "<mob>_head", "<mob>_wall_head":
     * the mob selects the texture, wall vs floor the cube and rotation.
     * piston_head misses the table and takes the normal path. */
    {
        int wall = 0; size_t mob_len = 0;
        if (ends_with(name, nlen, "_wall_skull", 11))      { wall = 1; mob_len = nlen - 11; }
        else if (ends_with(name, nlen, "_wall_head", 10))  { wall = 1; mob_len = nlen - 10; }
        else if (ends_with(name, nlen, "_skull", 6))       { wall = 0; mob_len = nlen - 6; }
        else if (ends_with(name, nlen, "_head", 5))        { wall = 0; mob_len = nlen - 5; }
        if (mob_len) {
            const skull_def_t *sd = NULL;
            for (size_t i = 0; i < sizeof(skull_defs) / sizeof(skull_defs[0]); i++) {
                if (skull_defs[i].mob_len == mob_len &&
                    memcmp(skull_defs[i].mob, name, mob_len) == 0) { sd = &skull_defs[i]; break; }
            }
            if (sd) {
                const canvas_t *tex = bm_load_texture_cached(reg, sd->tex, strlen(sd->tex));
                double rest = 0;
                int k = wall ? facing_k(props, prop_count) : rotation_k(props, prop_count, &rest);
                uint32_t n = head_elements(sd, tex, wall, accum, cap);
                if (k) for (uint32_t i = 0; i < n; i++) rotate_elem_y(&accum[i], k);
                *accum_count = n;
                spin_rest(accum, *accum_count, rest);
                *any_rot = (k != 0 || rest != 0);
                return 1;
            }
        }
    }

    /* ---- family: banner -------------------------------------------- */
    /* "<colour>_banner" (standing, rotation 0..15) or "<colour>_wall_banner"
     * (facing); the colour dyes the plain cloth. */
    {
        int wall = ends_with(name, nlen, "_wall_banner", 12);
        size_t clen = wall ? nlen - 12 : ends_with(name, nlen, "_banner", 7) ? nlen - 7 : 0;
        uint8_t dye[3];
        if (clen && blockmodel_dye_color(name, clen, dye)) {
            const canvas_t *tex = bm_load_texture_cached(reg, "entity/banner/banner_base", 25);
            double rest = 0;
            /* the layout faces south: facing south is no turn */
            int k = wall ? (facing_k(props, prop_count) + 2) & 3 : rotation_k(props, prop_count, &rest);
            const ent_cube_t *cubes = wall ? banner_wall_cubes : banner_standing_cubes;
            uint32_t n = wall ? 2 : 3;
            *accum_count = fill_cubes(cubes, n, tex, 64, 64, FLIP_Y | FLIP_Z, k, dye, accum, cap);
            spin_rest(accum, *accum_count, rest);
            *any_rot = (k != 0 || rest != 0);
            return 1;
        }
    }

    /* ---- decorated pot --------------------------------------------- */
    if (nlen == 13 && memcmp(name, "decorated_pot", 13) == 0 && cap >= 8) {
        const canvas_t *tex = bm_texture_get(reg, "prism:decorated_pot", 19);
        if (!tex) {
            canvas_t c = blockmodel_pot_texture(reg, NULL);
            if (c.pixels) tex = bm_texture_put(reg, "prism:decorated_pot", 19, &c);
        }
        uint32_t n = fill_cubes(pot_neck_cubes, 2, tex, POT_W, POT_H, FLIP_Y | FLIP_Z, 0, NULL, accum, cap);
        pot_plane(&accum[n++], tex, 1, 16, 1, 15, 16, 15, FACE_UP, 14, 13, 28, 27);
        pot_plane(&accum[n++], tex, 1, 0, 1, 15, 0, 15, FACE_DOWN, 0, 13, 14, 27);
        static const struct { double x0, z0, x1, z1; int f; } side[4] = {
            { 1, 1, 15, 1, FACE_NORTH }, { 1, 1, 1, 15, FACE_WEST }, { 15, 1, 15, 15, FACE_EAST }, { 1, 15, 15, 15, FACE_SOUTH },
        };
        for (int i = 0; i < 4; i++)
            pot_plane(&accum[n++], tex, side[i].x0, 0, side[i].z0, side[i].x1, 16, side[i].z1, side[i].f,
                      pot_side_at[i][0] + 1, pot_side_at[i][1], pot_side_at[i][0] + 15, pot_side_at[i][1] + 16);
        int k = facing_k(props, prop_count);
        if (k) for (uint32_t i = 0; i < n; i++) rotate_elem_y(&accum[i], k);
        *accum_count = n;
        *any_rot = k != 0;
        return 1;
    }

    /* ---- copper golem statue ---------------------------------------- */
    /* "[waxed_][exposed_|weathered_|oxidized_]copper_golem_statue", posed
     * by copper_golem_pose. CopperGolemStatueBlockRenderer: at the
     * block's middle, turned to face `facing`; the statue model turns its
     * root half round about z (upright, like a skull's scale(-1,-1,1)). */
    if (ends_with(name, nlen, "copper_golem_statue", 19)) {
        const char *st = name; size_t sl = nlen - 19;
        if (sl >= 6 && memcmp(st, "waxed_", 6) == 0) { st += 6; sl -= 6; }
        const char *tex_ref = sl == 0 ? "entity/copper_golem/copper_golem"
                            : sl == 8 && memcmp(st, "exposed_", 8) == 0 ? "entity/copper_golem/copper_golem_exposed"
                            : sl == 10 && memcmp(st, "weathered_", 10) == 0 ? "entity/copper_golem/copper_golem_weathered"
                            : sl == 9 && memcmp(st, "oxidized_", 9) == 0 ? "entity/copper_golem/copper_golem_oxidized" : NULL;
        if (tex_ref) {
            int pose = 0;
            for (size_t i = 0; i < prop_count; i++)
                if (props[i].name_len == 17 && memcmp(props[i].name, "copper_golem_pose", 17) == 0)
                    for (int j = 0; j < 4; j++)
                        if (strlen(golem_poses[j].pose) == props[i].value_len &&
                            memcmp(golem_poses[j].pose, props[i].value, props[i].value_len) == 0) pose = j;
            /* translate(8, 0, 8) * rotateY(facing, clockwise from north
             * seen from above) * rotateZ(pi) */
            int k = facing_k(props, prop_count);
            static const double cs[4][2] = { { 1, 0 }, { 0, -1 }, { -1, 0 }, { 0, 1 } }; /* cos, sin of -90k */
            double c = cs[k][0], s = cs[k][1];
            double root[3][4] = { { -c, 0, s, 8 }, { 0, -1, 0, 0 }, { s, 0, c, 8 } };
            const canvas_t *tex = bm_load_texture_cached(reg, tex_ref, strlen(tex_ref));
            *accum_count = posed_elements(golem_poses[pose].parts, golem_poses[pose].np, golem_poses[pose].cubes,
                                          golem_poses[pose].nc, root, tex, 64, 64, accum, cap);
            *any_rot = 1;
            return 1;
        }
    }

    /* ---- conduit ---------------------------------------------------- */
    /* ConduitRenderer: an inactive conduit is its shell, 6 wide, at the
     * block's middle; an active one (a cage and an eye, turning) is drawn
     * as inactive. */
    if (nlen == 7 && memcmp(name, "conduit", 7) == 0) {
        static const ent_cube_t shell = { 5, 5, 5, 6, 6, 6, 0, 0, 0, 0, 0 };
        const canvas_t *tex = bm_load_texture_cached(reg, "entity/conduit/base", 19);
        *accum_count = fill_cubes(&shell, 1, tex, 32, 16, 0u, 0, NULL, accum, cap);
        *any_rot = 0;
        return 1;
    }

    /* ---- end portal / gateway -------------------------------------- */
    /* The portal is drawn from 6/16 to 12/16 up, its top and bottom only;
     * the gateway is a whole cube. */
    {
        int portal = nlen == 10 && memcmp(name, "end_portal", 10) == 0;
        int gateway = nlen == 11 && memcmp(name, "end_gateway", 11) == 0;
        if ((portal || gateway) && cap >= 1) {
            const char *key = portal ? "prism:end_portal" : "prism:end_gateway";
            const canvas_t *tex = bm_texture_get(reg, key, strlen(key));
            if (!tex) {
                canvas_t c = portal_texture(reg, portal ? 15 : 16);
                if (c.pixels) tex = bm_texture_put(reg, key, strlen(key), &c);
            }
            blockmodel_element_t *e = &accum[0];
            memset(e, 0, sizeof *e);
            e->x1 = e->z1 = 16;
            e->y0 = portal ? 6 : 0; e->y1 = portal ? 12 : 16;
            e->rot_origin[0] = e->rot_origin[1] = e->rot_origin[2] = 8.0;
            for (int f = 0; f < FACE_COUNT; f++)
                if (!portal || f == FACE_UP || f == FACE_DOWN)
                    set_face(&e->faces[f], tex, 0, 0, PORTAL_N, PORTAL_N, PORTAL_N, PORTAL_N);
            *accum_count = 1;
            *any_rot = 0;
            return 1;
        }
    }

    /* ---- table: chests --------------------------------------------- */
    const ent_def_t *def = NULL;
    for (size_t i = 0; i < sizeof(entity_defs) / sizeof(entity_defs[0]); i++) {
        if (entity_defs[i].name_len == nlen && memcmp(entity_defs[i].name, name, nlen) == 0) {
            def = &entity_defs[i];
            break;
        }
    }
    if (!def) return 0;

    int type = def->dbl ? chest_type(props, prop_count) : 0;
    char ref[64];
    snprintf(ref, sizeof ref, "%s%s", def->tex, type == 1 ? "_left" : type == 2 ? "_right" : "");
    const canvas_t *tex = bm_load_texture_cached(reg, ref, strlen(ref));
    int k = facing_k(props, prop_count);
    *accum_count = fill_cubes(chest_cubes[type], 3, tex, 64, 64, 0u, k, NULL, accum, cap);
    *any_rot = (k != 0);
    return 1;
}
