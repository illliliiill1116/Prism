/* Byte buffers, and the full level 0 as one glTF binary (the "glb" option). */
#include "prism.h"

void bput(bytes_t *b, const void *src, size_t n) {
    if (b->n + n > b->c) {
        b->c = b->c ? b->c * 2 : 1 << 20;
        while (b->c < b->n + n) b->c *= 2;
        b->p = realloc(b->p, b->c);
        if (!b->p) { fprintf(stderr, "oom\n"); exit(1); }
    }
    memcpy(b->p + b->n, src, n);
    b->n += n;
}

void bpad(bytes_t *b, uint8_t v) { while (b->n % 4) bput(b, &v, 1); }

void sappend(str_t *s, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(s->p ? s->p + s->n : NULL, s->p ? s->c - s->n : 0, fmt, ap);
        va_end(ap);
        if (s->p && s->n + (size_t)k < s->c) { s->n += (size_t)k; return; }
        s->c = (s->c ? s->c : 4096) * 2 + (size_t)k;
        s->p = realloc(s->p, s->c);
        if (!s->p) { fprintf(stderr, "oom\n"); exit(1); }
    }
}

/* Quads per primitive: 4 vertices each, so uint16 indices reach 65536. */
#define PRIM_QUADS 16384

/* KHR_mesh_quantization layout: SHORT positions (stride 8), normalized
 * UNSIGNED_SHORT uvs, normalized UNSIGNED_BYTE colours. Every quad's
 * index pattern is the same, so one index buffer serves all primitives. */
int write_glb(const char *path, mbuf_t merged[MAT_COUNT], size_t tile_q0[MAT_COUNT][NTILES + 1],
                     uint32_t atlas_hash) {
    bytes_t bin = { 0 };
    str_t views = { 0 }, accs = { 0 }, prims = { 0 };
    int nview = 0, nacc = 0;
    static const char *mode[MAT_COUNT] = { "OPAQUE", "MASK", "BLEND", "BLEND" };
    static const char *mname[MAT_COUNT] = { "OPAQUE", "MASK", "BLEND", "WATER" };

    uint16_t *qidx = xmalloc(PRIM_QUADS * 6 * sizeof *qidx);
    for (uint32_t q = 0; q < PRIM_QUADS; q++) {
        uint16_t b = (uint16_t)(q * 4);
        uint16_t *I = &qidx[q * 6];
        I[0] = b; I[1] = (uint16_t)(b + 3); I[2] = (uint16_t)(b + 1);
        I[3] = (uint16_t)(b + 1); I[4] = (uint16_t)(b + 3); I[5] = (uint16_t)(b + 2);
    }
    size_t off = bin.n;
    bput(&bin, qidx, PRIM_QUADS * 6 * sizeof *qidx); bpad(&bin, 0);
    sappend(&views, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34963}", off, (size_t)PRIM_QUADS * 12);
    int vidx = nview++;
    free(qidx);

    for (int m = 0; m < MAT_COUNT; m++) {
        mbuf_t *b = &merged[m];
        if (!b->nv) continue;
        int16_t *pq = xmalloc(b->nv * 8);
        uint16_t *uq = xmalloc(b->nv * 4);
        for (size_t i = 0; i < b->nv; i++) {
            for (int a = 0; a < 3; a++) pq[i * 4 + a] = (int16_t)lrintf(b->pos[i * 3 + a] * POS_Q);
            pq[i * 4 + 3] = 0;
            for (int a = 0; a < 2; a++) {
                float v = b->uv[i * 2 + a];
                uq[i * 2 + a] = (uint16_t)lrintf((v < 0 ? 0 : v > 1 ? 1 : v) * 65535.0f);
            }
        }
        off = bin.n; bput(&bin, pq, b->nv * 8); bpad(&bin, 0);
        sappend(&views, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"byteStride\":8,\"target\":34962}", off, b->nv * 8);
        int vpos = nview++;
        off = bin.n; bput(&bin, uq, b->nv * 4); bpad(&bin, 0);
        sappend(&views, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"byteStride\":4,\"target\":34962}", off, b->nv * 4);
        int vuv = nview++;
        /* COLOR_0's alpha is opacity to a glTF reader: no emission there */
        uint8_t *cq = xmalloc(b->nv * VSTRIDE);
        memcpy(cq, b->col, b->nv * VSTRIDE);
        for (size_t i = 0; i < b->nv; i++) { cq[i * VSTRIDE + 3] = 255; cq[i * VSTRIDE + 15] &= 1; } /* _BLK alpha: hangs */
        off = bin.n; bput(&bin, cq, b->nv * VSTRIDE); bpad(&bin, 0);
        free(cq);
        sappend(&views, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"byteStride\":%d,\"target\":34962}", off, b->nv * VSTRIDE, VSTRIDE);
        int vcol = nview++;

        for (int t = 0; t < NTILES; t++)
        for (size_t q0 = tile_q0[m][t], qend = tile_q0[m][t + 1]; q0 < qend; q0 += PRIM_QUADS) {
            size_t nq = qend - q0 < PRIM_QUADS ? qend - q0 : PRIM_QUADS;
            size_t v0 = q0 * 4, nv = nq * 4;
            int mn[3] = { 32767, 32767, 32767 }, mx[3] = { -32768, -32768, -32768 };
            for (size_t i = v0; i < v0 + nv; i++)
                for (int a = 0; a < 3; a++) {
                    int v = pq[i * 4 + a];
                    if (v < mn[a]) mn[a] = v;
                    if (v > mx[a]) mx[a] = v;
                }
            int apos = nacc++, auv = nacc++, acol = nacc++, asun = nacc++, amask = nacc++, ablk = nacc++, abnc = nacc++, abamp = nacc++, aidx = nacc++;
            sappend(&accs,
                "%s{\"bufferView\":%d,\"byteOffset\":%zu,\"componentType\":5122,\"count\":%zu,\"type\":\"VEC3\",\"min\":[%d,%d,%d],\"max\":[%d,%d,%d]}"
                ",{\"bufferView\":%d,\"byteOffset\":%zu,\"componentType\":5123,\"normalized\":true,\"count\":%zu,\"type\":\"VEC2\"}"
                ",{\"bufferView\":%d,\"byteOffset\":%zu,\"componentType\":5121,\"normalized\":true,\"count\":%zu,\"type\":\"VEC4\"}"
                ",{\"bufferView\":%d,\"byteOffset\":%zu,\"componentType\":5121,\"normalized\":true,\"count\":%zu,\"type\":\"VEC4\"}"
                ",{\"bufferView\":%d,\"byteOffset\":%zu,\"componentType\":5121,\"normalized\":true,\"count\":%zu,\"type\":\"VEC4\"}"
                ",{\"bufferView\":%d,\"byteOffset\":%zu,\"componentType\":5121,\"normalized\":true,\"count\":%zu,\"type\":\"VEC4\"}"
                ",{\"bufferView\":%d,\"byteOffset\":%zu,\"componentType\":5121,\"normalized\":true,\"count\":%zu,\"type\":\"VEC4\"}"
                ",{\"bufferView\":%d,\"byteOffset\":%zu,\"componentType\":5121,\"normalized\":true,\"count\":%zu,\"type\":\"VEC4\"}"
                ",{\"bufferView\":%d,\"componentType\":5123,\"count\":%zu,\"type\":\"SCALAR\"}",
                apos ? "," : "", vpos, v0 * 8, nv, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2],
                vuv, v0 * 4, nv, vcol, v0 * VSTRIDE, nv, vcol, v0 * VSTRIDE + 4, nv, vcol, v0 * VSTRIDE + 8, nv, vcol, v0 * VSTRIDE + 12, nv,
                vcol, v0 * VSTRIDE + 16, nv, vcol, v0 * VSTRIDE + 20, nv, vidx, nq * 6);
            sappend(&prims, "%s{\"attributes\":{\"POSITION\":%d,\"TEXCOORD_0\":%d,\"COLOR_0\":%d,\"_SUN\":%d,\"_SUNWIN\":%d,\"_BLK\":%d,\"_BNC\":%d,\"_BAMP\":%d},\"indices\":%d,\"material\":%d}",
                    prims.n ? "," : "", apos, auv, acol, asun, amask, ablk, abnc, abamp, aidx, m);
        }
        free(pq); free(uq);
    }
    str_t json = { 0 };
    sappend(&json,
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"prism\"},"
        "\"extensionsUsed\":[\"KHR_materials_unlit\",\"KHR_mesh_quantization\"],"
        "\"extensionsRequired\":[\"KHR_mesh_quantization\"],"
        "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0,\"scale\":[%g,%g,%g]}],"
        "\"meshes\":[{\"primitives\":[%s]}],\"materials\":[",
        1 / POS_Q, 1 / POS_Q, 1 / POS_Q, prims.p ? prims.p : "");
    for (int m = 0; m < MAT_COUNT; m++)
        sappend(&json, "%s{\"name\":\"%s\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0,\"roughnessFactor\":1},"
                "\"alphaMode\":\"%s\"%s,\"doubleSided\":%s,\"extensions\":{\"KHR_materials_unlit\":{}}}",
                m ? "," : "", mname[m], mode[m], m == MAT_MASK ? ",\"alphaCutoff\":0.5" : "", m <= MAT_MASK ? "false" : "true");
    sappend(&json,
        "],\"textures\":[{\"source\":0,\"sampler\":0}],\"samplers\":[{\"magFilter\":9728,\"minFilter\":9987}],"
        "\"images\":[{\"uri\":\"atlas.png?%08x\"}],"
        "\"buffers\":[{\"byteLength\":%zu}],\"bufferViews\":[%s],\"accessors\":[%s]}",
        atlas_hash, bin.n, views.p, accs.p);
    while (json.n % 4) sappend(&json, " ");
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    uint32_t hdr[3] = { 0x46546C67u, 2, (uint32_t)(12 + 8 + json.n + 8 + bin.n) };
    uint32_t jh[2] = { (uint32_t)json.n, 0x4E4F534Au }, bh[2] = { (uint32_t)bin.n, 0x004E4942u };
    fwrite(hdr, 4, 3, f);
    fwrite(jh, 4, 2, f); fwrite(json.p, 1, json.n, f);
    fwrite(bh, 4, 2, f); fwrite(bin.p, 1, bin.n, f);
    free(bin.p); free(json.p); free(views.p); free(accs.p); free(prims.p);
    return fclose(f) != 0;
}
