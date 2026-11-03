/* prism's modules share these: the grid's geometry, the block states,
 * the world arrays the light and the mesher read, and each module's entry
 * points (grouped by the file that defines them). */
#ifndef PRISM_H
#define PRISM_H

#include "blockmodel.h"
#include "canvas.h"
#include "chunk.h"
#include "mca.h"
#include "nbt.h"
#include "png_image.h"
#include "json.h"
#include "world/tint.h"
#include "zip_reader.h"
#include "world/skin.h"
#include <zlib.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- shared ---- */

#define MARGIN 32
#define REG 512
#define GW (REG + 2 * MARGIN)
#define YMIN (-64)
#define GH 384
void *xmalloc(size_t n);
#define BOUNCE_ANGLES 4

/* Primitives never cross a tile of TILE_CHUNKS x TILE_CHUNKS chunks, so each
 * one's bounds are a tile and the viewer skips those out of view (one
 * primitive spread over the region is always in view). */
#define TILE_CHUNKS 4

#define NTILES ((32 / TILE_CHUNKS) * (32 / TILE_CHUNKS))

/* ---- world/states.c ---- */

enum { FLUID_NONE, FLUID_WATER, FLUID_LAVA };

typedef struct {
    char *key;
    const blockmodel_shape_t *shape;
    uint8_t air, fluid, fluid_block, fluid_h; /* fluid_block: water/lava itself, no model */
    uint8_t cull;   /* full opaque cube: hides any neighbour face against it */
    uint8_t self_cull; /* full cube that hides faces against the same state (glass) */
    uint8_t occl;   /* blocks light rays */
    uint8_t occ_y0, occ_y1; /* of those, the heights in it that do, 16ths: a slab's half */
    uint8_t far_occl; /* thin (a fence, a post): blocks the sun only far off (see ray_far_vis) */
    uint8_t tint_kind;      /* tint_kind_t from tint_classify */
    uint8_t tint_fixed[3];
    uint8_t leaves; /* cut out; faces against buried leaves are culled */
    uint8_t up_lit; /* thin cutout plant (is_plant): lit as if facing up, both sides alike */
    uint8_t jitter; /* the game's random XYZ offset (grass, ferns) */
    uint8_t emit;   /* block light given off, 0..15 */
    uint8_t be_tex; /* its block entity may give it its own texture (banner, player head, decorated pot) */
    float face_col[6][3]; /* L2: mean texture colour per face direction, linear, fixed tint applied */
    int8_t face_bc[6];    /* L2: biome colour family to multiply by, -1 none */
    uint32_t name_hash; /* block name only, for same-block culling across states */
} state_t;

extern state_t *states;
extern uint32_t nstates, cap_states;
extern uint32_t *htab; /* open addressing: state index + 1, 0 = empty */
extern uint32_t hcap;
uint32_t fnv(const char *s, size_t n);
extern blockmodel_registry_t *reg;
extern const canvas_t *tex_water, *tex_lava, *tex_white;
extern zip_archive_t jarzip;
uint32_t intern_state(const nbt_pool_t *pool, const nbt_tag_t *palette, uint32_t pi);
/* the same for a block state as NBT ({Name, Properties}), as a block
 * entity holds one */
uint32_t intern_block_state(const nbt_pool_t *pool, const nbt_tag_t *bs);

/* ---- world/atlas.c ---- */

typedef struct {
    const canvas_t *tex;
    uint32_t x, y, w, h; /* atlas cell origin; content is w x h at (x+w/2, y+h/2) */
    uint8_t has_zero, has_partial;
    float mean[3]; /* linear, over the texels that are not clear */
    float cover;   /* the share of texels that are not clear */
} texslot_t;

extern texslot_t *slots;
extern uint32_t nslots, cap_slots;
extern uint32_t *slot_hash;
extern uint32_t slot_hcap;
extern uint32_t atlas_w, atlas_h;
uint32_t tex_slot(const canvas_t *t);
int tex_opaque(const canvas_t *t);
canvas_t build_atlas(void);

/* ---- world/world.c ---- */

extern uint16_t *grid; /* state index, 0 = air (state 0 is interned as air) */

/* Biomes: one id per 4x4x4 cell, as the game stores them. Colours are
 * linear RGB: grass, foliage, dry foliage, water. */
#define BW (GW / 4)

#define BH (GH / 4)
enum { BC_GRASS, BC_FOLIAGE, BC_DRY, BC_WATER, BC_COUNT };
extern int nbiomes;
extern uint8_t *bgrid;
extern tint_ctx_t *tints;
float lin_to_srgb(float c);
float srgb_to_lin(float c);
void rgb8_lin(const uint8_t c[3], float out[3]);
void biome_color_at(int bc, const float p[3], float out[3]);
extern uint64_t *occl; /* 1 bit per voxel: blocks light */
extern uint64_t *focc; /* 1 bit per voxel: a thin blocker (far_occl) */
static inline size_t gidx(int x, int y, int z) { return ((size_t)y * GW + (size_t)z) * GW + (size_t)x; }

static inline uint16_t gget(int x, int y, int z) {
    if (x < 0 || z < 0 || x >= GW || z >= GW || y < 0 || y >= GH) return 0;
    return grid[gidx(x, y, z)];
}

static inline int occ_at(int x, int y, int z) {
    size_t i = gidx(x, y, z);
    return (int)((occl[i >> 6] >> (i & 63)) & 1);
}

/* An occluding voxel that the ray through it, from t0 to t1, misses: one
 * blocking at some heights only (occ_y0..occ_y1, a slab) that the ray
 * passes above or below. */
static inline int occ_miss(int x, int y, int z, const float o[3], const float d[3], float t0, float t1) {
    const state_t *st = &states[grid[gidx(x, y, z)]];
    if (st->occ_y1 - st->occ_y0 >= 16) return 0;
    float ya = o[1] + d[1] * t0 - y, yb = o[1] + d[1] * t1 - y;
    if (ya > yb) { float k = ya; ya = yb; yb = k; }
    return yb <= st->occ_y0 / 16.0f || ya >= st->occ_y1 / 16.0f;
}

extern uint64_t *pocc;

static inline int pocc_at(int x, int y, int z) {
    size_t i = gidx(x, y, z);
    return (int)((pocc[i >> 6] >> (i & 63)) & 1);
}

static inline int focc_at(int x, int y, int z) {
    size_t i = gidx(x, y, z);
    return (int)((focc[i >> 6] >> (i & 63)) & 1);
}

extern long chunks_loaded;
extern uint8_t *skyl;
/* Caves are dropped below cave_y: CAVE_Y (as BlueMap's default), or
 * lower on a map whose water is lower (see bake_region). */
#define CAVE_Y 55
extern int cave_y;
extern int16_t *coltop;

static inline int sky_at(int x, int y, int z) {
    if (y >= GH || x < 0 || z < 0 || x >= GW || z >= GW) return 15;
    if (y < 0) return 0;
    if (y > coltop[z * GW + x]) return 15;
    size_t i = gidx(x, y, z);
    return (skyl[i >> 1] >> ((i & 1) * 4)) & 15;
}

extern uint8_t *blkl;

/* The emitting voxels again, by EMIT_CELL^3 cell of the grid (CSR: cell
 * c's are emv[emc[c]..emc[c+1]]), for the bake's rays to each light. */
#define EMIT_CELL 16

#define EMIT_CW (GW / EMIT_CELL)
#define EMIT_CH (GH / EMIT_CELL)
typedef struct { uint16_t x, y, z; uint8_t e; } emitter_t;
extern emitter_t *emv;
extern uint32_t emc[EMIT_CW * EMIT_CW * EMIT_CH + 1];

/* A voxel below cave_y that no sky light reaches. */
static inline int is_cave(int x, int y, int z) {
    return y + YMIN < cave_y && sky_at(x, y, z) == 0;
}

/* A chunk's NBT (MCA_OK; MCA_ERR_EMPTY if it is not there, another
 * mca_result_t if it could not be read): thread-safe. load_chunk
 * then puts it in the grid at gx0, gz0 (one thread: it interns states)
 * and frees it. */
int parse_chunk(const mca_region_t *r, int lx, int lz, nbt_pool_t *pool, nbt_tag_t **root);
void load_chunk(nbt_pool_t *pool, nbt_tag_t *root, int gx0, int gz0);
long propagate_sky(void);
long propagate_block(void);

/* ---- world/entities.c ---- */

/* A banner's patterns and a player head's skin replace the texture its
 * shape (blockmodel's entity_shapes.c) draws with - the plain banner cloth,
 * Steve - keeping that texture's layout. One canvas per distinct look,
 * found by grid position. */
typedef struct { char *key; canvas_t tex; } betex_t;

extern betex_t *betex;
extern uint32_t nbetex, cbetex;
typedef struct { uint32_t gi, tex; } bepos_t; /* gi + 1, 0 = empty; betex index */
extern bepos_t *bepos;
extern uint32_t bepos_cap, nbepos;
extern const char *skin_dir;
extern int reg_x0, reg_z0; /* world x/z of grid cell 0 (set in main) */
extern long be_banners, be_heads, be_missing_skins, be_pots, be_moving;
const canvas_t *be_texture(int gx, int gy, int gz);
void load_block_entities(const nbt_pool_t *pool, const nbt_tag_t *root);

/* ---- light/light.c ---- */

extern int occ_top; /* highest grid y holding an occluder */
#define SKY_RAYS 16
#define VSTRIDE 24 /* vertex colour bytes: COLOR_0, _SUN, _SUNWIN, _BLK, _BNC, _BAMP (see bounce) */
#define PSTRIDE 12 /* sky rays open + 32 x wet samples (255: the colours are kept as they are), block

                      light, tint rgb, shade, water: light through it (see bake), its colour rgb */
#define THIN_ALPHA 191

#define LEAF_ALPHA 240

/* A plant standing on the ground (grass, flowers) is darker at its foot,
 * where the blades shade each other: its bottom corners' tint is scaled by
 * PLANT_BASE (linear), a gradient up the blade as shader packs draw it. */
#define PLANT_BASE 0.40f

void sun_at(float th, float d[3]);
void normalize3(float v[3]);
void setup_dirs(void);

/* Empty-space skipping for rays that do not go down: the highest
 * occluder in each HM_CELL x HM_CELL column of the grid, and in each
 * HM_BIG one (-1 for none). A ray in a voxel above its column's can go
 * straight to the column's edge. */
#define HM_CELL 8

#define HM_W (GW / HM_CELL)
#define HM_BIG 32
#define HM_BW ((GW + HM_BIG - 1) / HM_BIG)
extern int16_t hm_top[HM_W * HM_W], hm_big[HM_BW * HM_BW];
int ray_free(const float o[3], const float d[3], float maxd, int leaf);
static const float SUN_COL[3] = { 0.78f, 0.60f, 0.42f }; /* warm, on flat lit ground */
static const float BLOCK_COL[3] = { 1.50f, 0.88f, 0.38f }; /* torchlight at full brightness */
#define BLOCK_DAY 0.5f /* share of it in COLOR_0: daylight drowns it out */
#define BLOCK_MAX 1.9f
static const float SKY_COL[3] = { 0.54f, 0.52f, 0.61f }; /* cool, under open sky: a fifth of the 2.5D violet */
#define SKY_FLOOR 0.20f /* share of skylight that reaches anywhere (the sky's bounce) */
#define LIGHT_MIN 0.25f
#define LIGHT_MAX 1.40f
#define ALBEDO 0.88f    /* the 2.5D LIGHT_ALBEDO; lit ground sums a bit over 1 */

/* prism iso: the sun at one point of its arc, th (0..pi), not all of it:
 * a corner's sun window is then the whole arc or none. The bounce stays
 * the arc's mean, as the viewer has it. sun_only < 0 (the default) is the
 * whole arc. */
extern float sun_only;
void light_one_sun(float th);
float face_shade(const float n[3], int leaf);

/* A corner's light (bake and sun_window) depends on nothing but where it
 * is and the normal it is lit by, and up to four faces share each corner,
 * as does every level after the first: so it is worked out once, kept in
 * an open-addressed table shared by the meshing threads. A slot goes empty
 * -> being written -> ready; a reader takes only ready ones, and a writer
 * that finds a slot being written does not wait but leaves that corner
 * uncached. Only the six axis normals are kept (the key is the float bits
 * of the position and the axis). */
#define LC_BITS 22 /* the table's size, lc_bits, by default: 2^22 slots */
extern int lc_bits;

typedef struct {
    _Atomic uint32_t state; /* 0 empty, 1 being written, 2 ready */
    uint32_t key[4];
    float L[5][3];
    uint16_t bq[BOUNCE_ANGLES][3]; /* bounce(), in 1/BQ_ONE */
    uint8_t swin[4], nwin;
} lcache_t;

extern lcache_t *lcache;
int corner_light(const float p[3], const float n[3], int leaf, float L[5][3], uint8_t swin[4], float B[BOUNCE_ANGLES][3]);

/* ---- light/bounce.c ---- */

static const int DIRV[FACE_COUNT][3] = {
    { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { 1, 0, 0 }, { -1, 0, 0 },
};

#define BOUNCE_MAX 0.5f /* _BAMP's full scale, in COLOR_0's half-scale units */
#define BNC_Q 7   /* _BNC levels */
#define BAMP_Q 31 /* _BAMP levels, of the square root: most bounce is faint */
#define BAMP_MIN 2 /* fainter is dropped: its colour cost bytes nobody sees */
extern float bounce_sun[BOUNCE_ANGLES][3];
void setup_bounce(void);

/* rays a corner's bounce is gathered over, up to (and by default) the
 * bake's 48: prism iso takes fewer, a noisier bounce for less time */
extern int bounce_rays;
void set_bounce_rays(int n);

/* A face's light to bounce, cached by face: its colour (linear, biome tint
 * applied) and the sun on it at each angle, in 255ths. Slots go empty ->
 * being written -> ready as the corner cache's do. */
#define FC_BITS 22 /* fc_bits by default */
extern int fc_bits;

typedef struct {
    _Atomic uint32_t state;
    uint32_t key;
    float col[3];
    uint8_t e[BOUNCE_ANGLES];
} fcache_t;

extern fcache_t *fcache;
void bounce(const float p[3], const float n[3], float B[BOUNCE_ANGLES][3]);

/* ---- mesh/mesh.c ---- */

/* How a plant shows far off (mesh_far_plant), where level 0's crosses
 * are a few pixels: PLANT_CANOPY, not ground cover (its column of plants
 * does not stand on a full block: it grows from leaves, hangs, floats,
 * as maps use grass and kelp for foliage), as a leaf block;
 * PLANT_SUNK, under water (a kelp forest, seagrass), seen through the
 * water from above, by its column's top; else not at all (grass on the
 * ground, whose own colour is there). Level 0 keeps the first two past
 * the viewer's plant fade. */
enum { PLANT_FADES, PLANT_CANOPY, PLANT_SUNK };

int plant_far(int bx, int by, int bz);
enum { MAT_OPAQUE, MAT_MASK, MAT_BLEND, MAT_WATER, MAT_COUNT };
extern int lod_mode;
/* chunks of the region with nothing in them (mesh_skip), and the rows
 * below mesh_y0 empty: window_clear_outside's, which mesh_pass skips */
extern uint8_t mesh_skip[32 * 32];
extern int mesh_y0;

/* prism iso: mesh unlit (light_defer), keeping in dfr what each corner's
 * light needs, and light only the quads that show (light_quad), once it
 * is known which. 0 (the default) lights every quad as it is meshed. */
extern int light_defer;
#define DSTRIDE 7 /* dfr floats per vertex: the corner, its tint, up_lit | leaf << 1 */

/* What colours a face: none, a biome family (per vertex), or fixed. */
enum { PAINT_NONE, PAINT_BIOME, PAINT_FIXED };

typedef struct { int kind, bc; float fixed[3]; } paint_t;

typedef struct {
    float *pos, *uv;
    uint8_t *col, *par; /* par: PSTRIDE per vertex, what the colours are made of (see tile_payload) */
    float *dfr; /* light_defer only */
    uint32_t *idx;
    size_t nv, cv, ni, ci;
} mbuf_t;

void emit_quad(mbuf_t *m, float q[4][3], const canvas_t *tex, const float uv16[4],
                      int uvrot, const paint_t *pt, int up_lit, int leaf, int emit, long *nquads);

void box_face(int f, const float b[6], float q[4][3]);

typedef struct {
    mbuf_t mb[MAT_COUNT];
    long quads;
} worker_t;

float far_plant_dim(const state_t *st);
float sunk_cap(int x, int y, int z);
long mesh_pass(int nthreads, mbuf_t merged[MAT_COUNT], size_t tile_q0[MAT_COUNT][NTILES + 1]);
void mb_free(mbuf_t mb[MAT_COUNT]);
void light_quad(mbuf_t *m, size_t lq); /* a light_defer quad, lq of m, lit */

/* ---- mesh/coarse.c ---- */

void prepare_face_colors(void);
void mesh_coarse(worker_t *w, int cx, int cz);

/* ---- tiles/glb.c ---- */

typedef struct { uint8_t *p; size_t n, c; } bytes_t;
void bput(bytes_t *b, const void *src, size_t n);
void bpad(bytes_t *b, uint8_t v);
typedef struct { char *p; size_t n, c; } str_t;
void sappend(str_t *s, const char *fmt, ...);
#define POS_Q 64.0f /* positions stored in 1/64 block, scaled back by the node */

int write_glb(const char *path, mbuf_t merged[MAT_COUNT], size_t tile_q0[MAT_COUNT][NTILES + 1],
                     uint32_t atlas_hash);

/* ---- tiles/tiles.c ---- */


/* The map's format: what map.json and every index.json say they are in
 * ("format"), which the viewer checks. Bump it with any change the viewer
 * of the time could not read. */
#define MAP_FORMAT 1
#define TILES_FILE "tiles.bin"

int pack_level_tiles(mbuf_t m0[MAT_COUNT], size_t q0[MAT_COUNT][NTILES + 1], mbuf_t m1[MAT_COUNT],
                     size_t q1[MAT_COUNT][NTILES + 1], size_t *quads[3], bytes_t gzs[NTILES][3], int t0, int t1,
                     int nthreads);
int pack_level2(mbuf_t merged[MAT_COUNT], size_t tile_q0[MAT_COUNT][NTILES + 1], size_t quads[NTILES],
                bytes_t *l2, int nthreads);
int write_tiles(const char *path, bytes_t *l2, bytes_t gzs[NTILES][3], uint64_t at[NTILES][4]);

/* ---- world/window.c ---- */

int prism_open(const char *jar);
/* empties the light caches (lcache, fcache), allocated at their sizes */
void light_caches_clear(void);
void window_load(const char *rdir, int c0x, int c0z, canvas_t *atlas, long *lit, long *emitters);
void window_clear_outside(int x0, int z0, int x1, int z1, int ymin);

/* ---- iso/isodraw.c ---- */

typedef struct { int scale, ss, shadows, threads; float sun; } iso_opts_t;

/* An image drawn window by window: pixel (0, 0) is projected pixel ox, oy
 * (iso_project); a row is allocated when something is drawn in it, and
 * x0..x1 is what was. */
typedef struct {
    int w, h;
    long ox, oy;
    uint8_t **rows;
    int *x0, *x1;
} iso_image_t;

void iso_project(int scale, double wx, double wy, double wz, double *sx, double *sy);
int iso_image_init(iso_image_t *img, int scale, int x0, int z0, int x1, int z1, int ymin);
void iso_image_free(iso_image_t *img);
int iso_image_write(const iso_image_t *img, const char *path, int nthreads, long box[4]);
int iso_draw(iso_image_t *img, mbuf_t mb[MAT_COUNT], const canvas_t *atlas, const iso_opts_t *o, const int core[4]);

#endif
