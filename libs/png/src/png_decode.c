#include "png_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

static const uint8_t kSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

static uint32_t rd_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* ------------------------------------------------------------------- */
/* raw zlib inflate into a growable buffer                              */
/* ------------------------------------------------------------------- */

static png_result_t inflate_zlib(const uint8_t *in, size_t in_len, uint8_t **out_buf, size_t *out_len) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (inflateInit(&strm) != Z_OK) return PNG_ERR_INFLATE;

    size_t cap = in_len * 4 > 4096 ? in_len * 4 : 4096;
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) { inflateEnd(&strm); return PNG_ERR_OOM; }

    strm.next_in = (Bytef *)in;
    strm.avail_in = (uInt)in_len;

    size_t total = 0;
    int zret;
    do {
        if (total == cap) {
            size_t new_cap = cap * 2;
            uint8_t *grown = (uint8_t *)realloc(out, new_cap);
            if (!grown) { free(out); inflateEnd(&strm); return PNG_ERR_OOM; }
            out = grown;
            cap = new_cap;
        }
        strm.next_out = out + total;
        strm.avail_out = (uInt)(cap - total);
        zret = inflate(&strm, Z_NO_FLUSH);
        if (zret != Z_OK && zret != Z_STREAM_END) {
            free(out);
            inflateEnd(&strm);
            return PNG_ERR_INFLATE;
        }
        total = cap - strm.avail_out;
    } while (zret != Z_STREAM_END);

    inflateEnd(&strm);
    *out_buf = out;
    *out_len = total;
    return PNG_OK;
}

/* ------------------------------------------------------------------- */
/* chunk-level parsing                                                  */
/* ------------------------------------------------------------------- */

typedef struct {
    uint32_t width, height;
    uint8_t bit_depth, color_type;
    const uint8_t *palette;    /* PLTE data, 3 bytes per entry, or NULL */
    uint32_t palette_count;
    const uint8_t *trns;       /* tRNS data, or NULL */
    uint32_t trns_len;
    uint8_t *idat;             /* concatenated IDAT payloads */
    size_t idat_len, idat_cap;
} png_meta_t;

static int channels_for_color_type(uint8_t color_type) {
    switch (color_type) {
    case 0: return 1; /* gray */
    case 2: return 3; /* RGB */
    case 3: return 1; /* palette index */
    case 4: return 2; /* gray+alpha */
    case 6: return 4; /* RGBA */
    default: return -1;
    }
}

static png_result_t append_idat(png_meta_t *m, const uint8_t *data, size_t len) {
    if (m->idat_len + len > m->idat_cap) {
        size_t new_cap = m->idat_cap ? m->idat_cap * 2 : 65536;
        while (new_cap < m->idat_len + len) new_cap *= 2;
        uint8_t *grown = (uint8_t *)realloc(m->idat, new_cap);
        if (!grown) return PNG_ERR_OOM;
        m->idat = grown;
        m->idat_cap = new_cap;
    }
    memcpy(m->idat + m->idat_len, data, len);
    m->idat_len += len;
    return PNG_OK;
}

/* PNG paeth predictor, straight out of the spec. */
static uint8_t paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    if (pb <= pc) return (uint8_t)b;
    return (uint8_t)c;
}

static void unfilter_row(uint8_t *row, const uint8_t *prev_row, size_t row_bytes, int bpp, uint8_t filter_type) {
    for (size_t i = 0; i < row_bytes; i++) {
        int a = (i >= (size_t)bpp) ? row[i - bpp] : 0;
        int b = prev_row ? prev_row[i] : 0;
        int c = (prev_row && i >= (size_t)bpp) ? prev_row[i - bpp] : 0;
        int raw = row[i];
        switch (filter_type) {
        case 0: break; /* None */
        case 1: raw = (raw + a) & 0xFF; break; /* Sub */
        case 2: raw = (raw + b) & 0xFF; break; /* Up */
        case 3: raw = (raw + (a + b) / 2) & 0xFF; break; /* Average */
        case 4: raw = (raw + paeth(a, b, c)) & 0xFF; break; /* Paeth */
        default: break;
        }
        row[i] = (uint8_t)raw;
    }
}

/* Extracts sample `index` (0-based) from a bit-packed row for bit_depth
 * in {1,2,4,8}, MSB-first per PNG spec. */
static uint32_t extract_sample(const uint8_t *row, uint32_t index, uint8_t bit_depth) {
    if (bit_depth == 8) return row[index];
    uint32_t samples_per_byte = 8 / bit_depth;
    uint32_t byte_index = index / samples_per_byte;
    uint32_t sample_in_byte = index % samples_per_byte;
    uint32_t shift = 8 - bit_depth - sample_in_byte * bit_depth;
    uint32_t mask = (1u << bit_depth) - 1;
    return (row[byte_index] >> shift) & mask;
}

png_result_t png_decode(const uint8_t *data, size_t len, canvas_t *out) {
    if (len < 8 || memcmp(data, kSignature, 8) != 0) return PNG_ERR_BAD_SIGNATURE;

    png_meta_t m;
    memset(&m, 0, sizeof(m));
    int have_ihdr = 0;

    size_t pos = 8;
    while (pos + 8 <= len) {
        uint32_t chunk_len = rd_u32be(data + pos);
        const uint8_t *type = data + pos + 4;
        if (pos + 12 + chunk_len > len) { free(m.idat); return PNG_ERR_TRUNCATED; }
        const uint8_t *chunk_data = data + pos + 8;
        uint32_t stored_crc = rd_u32be(data + pos + 8 + chunk_len);

        uLong crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, type, 4);
        crc = crc32(crc, chunk_data, chunk_len);
        if ((uLong)stored_crc != crc) { free(m.idat); return PNG_ERR_CRC_MISMATCH; }

        if (memcmp(type, "IHDR", 4) == 0) {
            if (chunk_len != 13) { free(m.idat); return PNG_ERR_BAD_CHUNK; }
            m.width = rd_u32be(chunk_data);
            m.height = rd_u32be(chunk_data + 4);
            m.bit_depth = chunk_data[8];
            m.color_type = chunk_data[9];
            uint8_t compression = chunk_data[10], filter = chunk_data[11], interlace = chunk_data[12];
            if (compression != 0 || filter != 0 || interlace != 0) { free(m.idat); return PNG_ERR_UNSUPPORTED; }
            if (channels_for_color_type(m.color_type) < 0) { free(m.idat); return PNG_ERR_UNSUPPORTED; }
            if (m.bit_depth != 1 && m.bit_depth != 2 && m.bit_depth != 4 && m.bit_depth != 8) {
                free(m.idat);
                return PNG_ERR_UNSUPPORTED;
            }
            if (m.color_type != 0 && m.color_type != 3 && m.bit_depth != 8) {
                /* RGB/gray+alpha/RGBA only ever appear at 8 bits in the wild
                 * and per spec only support 8/16 anyway; 16 is rejected above. */
                free(m.idat);
                return PNG_ERR_UNSUPPORTED;
            }
            have_ihdr = 1;
        } else if (memcmp(type, "PLTE", 4) == 0) {
            m.palette = chunk_data;
            m.palette_count = chunk_len / 3;
        } else if (memcmp(type, "tRNS", 4) == 0) {
            m.trns = chunk_data;
            m.trns_len = chunk_len;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            png_result_t r = append_idat(&m, chunk_data, chunk_len);
            if (r != PNG_OK) { free(m.idat); return r; }
        } else if (memcmp(type, "IEND", 4) == 0) {
            pos += 12 + chunk_len;
            break;
        }

        pos += 12 + chunk_len;
    }

    if (!have_ihdr) { free(m.idat); return PNG_ERR_NO_IHDR; }
    if (!m.idat) return PNG_ERR_NO_IDAT;
    if (m.color_type == 3 && !m.palette) { free(m.idat); return PNG_ERR_NO_PLTE; }

    uint8_t *inflated;
    size_t inflated_len;
    png_result_t r = inflate_zlib(m.idat, m.idat_len, &inflated, &inflated_len);
    free(m.idat);
    if (r != PNG_OK) return r;

    int channels = channels_for_color_type(m.color_type);
    size_t bits_per_pixel = (size_t)channels * m.bit_depth;
    size_t row_bytes = (m.width * bits_per_pixel + 7) / 8;
    int bpp_for_filter = (int)((bits_per_pixel + 7) / 8);
    if (bpp_for_filter < 1) bpp_for_filter = 1;

    if (inflated_len < (size_t)m.height * (row_bytes + 1)) {
        free(inflated);
        return PNG_ERR_TRUNCATED;
    }

    uint8_t *rgba = (uint8_t *)malloc((size_t)m.width * m.height * 4);
    if (!rgba) { free(inflated); return PNG_ERR_OOM; }

    uint8_t *prev_row = NULL;
    const uint8_t *src = inflated;
    uint32_t max_val = (1u << m.bit_depth) - 1;

    for (uint32_t y = 0; y < m.height; y++) {
        uint8_t filter_type = src[0];
        uint8_t *row = (uint8_t *)(src + 1);
        unfilter_row(row, prev_row, row_bytes, bpp_for_filter, filter_type);

        uint8_t *out_row = rgba + (size_t)y * m.width * 4;
        for (uint32_t x = 0; x < m.width; x++) {
            uint8_t r8, g8, b8, a8 = 255;

            if (m.color_type == 0) {
                uint32_t v = extract_sample(row, x, m.bit_depth);
                uint8_t scaled = (uint8_t)((v * 255u) / max_val);
                r8 = g8 = b8 = scaled;
                if (m.trns && m.trns_len >= 2) {
                    uint32_t key = ((uint32_t)m.trns[0] << 8) | m.trns[1];
                    if (v == key) a8 = 0;
                }
            } else if (m.color_type == 2) {
                r8 = row[x * 3 + 0];
                g8 = row[x * 3 + 1];
                b8 = row[x * 3 + 2];
                if (m.trns && m.trns_len >= 6) {
                    uint32_t kr = ((uint32_t)m.trns[0] << 8) | m.trns[1];
                    uint32_t kg = ((uint32_t)m.trns[2] << 8) | m.trns[3];
                    uint32_t kb = ((uint32_t)m.trns[4] << 8) | m.trns[5];
                    if (r8 == kr && g8 == kg && b8 == kb) a8 = 0;
                }
            } else if (m.color_type == 3) {
                uint32_t idx = extract_sample(row, x, m.bit_depth);
                if (idx >= m.palette_count) { free(rgba); free(inflated); return PNG_ERR_BAD_CHUNK; }
                r8 = m.palette[idx * 3 + 0];
                g8 = m.palette[idx * 3 + 1];
                b8 = m.palette[idx * 3 + 2];
                a8 = (m.trns && idx < m.trns_len) ? m.trns[idx] : 255;
            } else if (m.color_type == 4) {
                r8 = g8 = b8 = row[x * 2 + 0];
                a8 = row[x * 2 + 1];
            } else { /* color_type == 6 */
                r8 = row[x * 4 + 0];
                g8 = row[x * 4 + 1];
                b8 = row[x * 4 + 2];
                a8 = row[x * 4 + 3];
            }

            out_row[x * 4 + 0] = r8;
            out_row[x * 4 + 1] = g8;
            out_row[x * 4 + 2] = b8;
            out_row[x * 4 + 3] = a8;
        }

        prev_row = row;
        src += 1 + row_bytes;
    }

    free(inflated);
    out->width = m.width;
    out->height = m.height;
    out->stride = (ptrdiff_t)m.width * 4;
    out->pixels = rgba;
    out->owns_pixels = 1;
    return PNG_OK;
}

png_result_t png_decode_file(const char *path, canvas_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return PNG_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return PNG_ERR_IO; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return PNG_ERR_IO; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return PNG_ERR_IO; }

    uint8_t *buf = (uint8_t *)malloc((size_t)size ? (size_t)size : 1);
    if (!buf) { fclose(f); return PNG_ERR_OOM; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(buf); return PNG_ERR_IO; }

    png_result_t r = png_decode(buf, (size_t)size, out);
    free(buf);
    return r;
}
