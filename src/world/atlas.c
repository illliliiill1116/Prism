/* Textures: every texture a face uses gets a cell in one atlas, padded so
 * mipmaps do not bleed, with its mean colour and coverage kept for the
 * coarse level and the far plants. */
#include "prism.h"

texslot_t *slots;

uint32_t nslots, cap_slots;

uint32_t *slot_hash;

uint32_t slot_hcap = 1u << 13;

uint32_t atlas_w, atlas_h;

uint32_t tex_slot(const canvas_t *t) {
    uint32_t h = (uint32_t)(((uintptr_t)t >> 4) * 2654435761u) & (slot_hcap - 1);
    for (;;) {
        uint32_t v = slot_hash[h];
        if (!v) break;
        if (slots[v - 1].tex == t) return v - 1;
        h = (h + 1) & (slot_hcap - 1);
    }
    if (nslots == cap_slots) {
        cap_slots = cap_slots ? cap_slots * 2 : 256;
        slots = realloc(slots, cap_slots * sizeof *slots);
    }
    texslot_t *s = &slots[nslots];
    memset(s, 0, sizeof *s);
    s->tex = t;
    /* a strip of square frames (animated) shows its first; an entity
     * texture may be wider than tall (a 64x32 skull) */
    s->w = t->width;
    s->h = t->height > t->width ? t->width : t->height;
    double sum[3] = { 0 }, cnt = 0;
    for (uint32_t y = 0; y < s->h; y++) {
        const uint8_t *row = canvas_row(t, y);
        for (uint32_t x = 0; x < s->w; x++) {
            uint8_t a = row[x * 4 + 3];
            if (a == 0) { s->has_zero = 1; continue; }
            if (a < 255) s->has_partial = 1;
            for (int k = 0; k < 3; k++) sum[k] += srgb_to_lin(row[x * 4 + k] / 255.0f);
            cnt++;
        }
    }
    for (int k = 0; k < 3; k++) s->mean[k] = cnt ? (float)(sum[k] / cnt) : 0.5f;
    s->cover = (float)(cnt / ((double)s->w * s->h));
    slot_hash[h] = ++nslots;
    return nslots - 1;
}

/* tex_slot may realloc slots: always take the index first, never
 * &slots[tex_slot(t)], whose slots can be read before the call moves it. */
int tex_opaque(const canvas_t *t) {
    uint32_t i = tex_slot(t);
    const texslot_t *s = &slots[i];
    return !s->has_zero && !s->has_partial;
}

static int cmp_slot_h(const void *a, const void *b) {
    const texslot_t *x = *(texslot_t *const *)a, *y = *(texslot_t *const *)b;
    return (int)y->h - (int)x->h;
}

/* Shelf packing, tallest first, into a power-of-two atlas. */
canvas_t build_atlas(void) {
    texslot_t **order = xmalloc(nslots * sizeof *order);
    for (uint32_t i = 0; i < nslots; i++) order[i] = &slots[i];
    qsort(order, nslots, sizeof *order, cmp_slot_h);
    atlas_w = 2048;
    uint32_t x = 0, y = 0, shelf = 0;
    for (uint32_t i = 0; i < nslots; i++) {
        texslot_t *s = order[i];
        uint32_t cell = s->w * 2;
        if (x + cell > atlas_w) { x = 0; y += shelf; shelf = 0; }
        s->x = x; s->y = y;
        x += cell;
        if (s->h * 2 > shelf) shelf = s->h * 2;
    }
    atlas_h = 16;
    while (atlas_h < y + shelf) atlas_h *= 2;
    canvas_t a = canvas_create(atlas_w, atlas_h);
    /* Each texture is surrounded by half its size of its own edge pixels,
     * so mip levels down to one texel per texture never mix neighbours. */
    for (uint32_t i = 0; i < nslots; i++) {
        texslot_t *s = &slots[i];
        int w = (int)s->w, h = (int)s->h, padx = w / 2, pady = h / 2;
        for (int r = 0; r < 2 * h; r++) {
            int sr = r - pady < 0 ? 0 : r - pady >= h ? h - 1 : r - pady;
            uint8_t *dst = canvas_row(&a, s->y + (uint32_t)r) + s->x * 4;
            const uint8_t *src = canvas_row(s->tex, (uint32_t)sr);
            for (int c = 0; c < 2 * w; c++) {
                int sc = c - padx < 0 ? 0 : c - padx >= w ? w - 1 : c - padx;
                memcpy(dst + c * 4, src + sc * 4, 4);
            }
        }
        if (!s->has_zero) continue;
        /* Clear texels keep the texture's mean colour, not black, so mip
         * levels do not darken cutout plants. */
        uint64_t sum[3] = { 0 }, cnt = 0;
        for (uint32_t r = 0; r < s->h; r++) {
            const uint8_t *src = canvas_row(s->tex, r);
            for (uint32_t c = 0; c < s->w; c++)
                if (src[c * 4 + 3]) { for (int k = 0; k < 3; k++) sum[k] += src[c * 4 + k]; cnt++; }
        }
        if (!cnt) continue;
        for (int r = 0; r < 2 * h; r++) {
            uint8_t *dst = canvas_row(&a, s->y + (uint32_t)r) + s->x * 4;
            for (int c = 0; c < 2 * w; c++)
                if (!dst[c * 4 + 3]) for (int k = 0; k < 3; k++) dst[c * 4 + k] = (uint8_t)(sum[k] / cnt);
        }
    }
    free(order);
    return a;
}
