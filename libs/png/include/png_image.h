/*
 * png_image.h - a minimal PNG codec for block textures and rendered
 * images, reading into and writing out of canvas_t.
 *
 * Decoder (covers every texture in a 26.2 client jar):
 *   - colour types 0, 2, 3, 4, 6; bit depths 1, 2, 4, 8 (not 16)
 *   - PLTE + tRNS
 *   - non-interlaced only (no Adam7)
 *   - multiple IDAT chunks
 *
 * Encoder: 8-bit RGBA, filter None, through `stride`, so a subview encodes
 * without a copy.
 */
#ifndef PRISM_PNG_IMAGE_H
#define PRISM_PNG_IMAGE_H

#include "canvas.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum png_result {
    PNG_OK = 0,
    PNG_ERR_IO,
    PNG_ERR_BAD_SIGNATURE,
    PNG_ERR_BAD_CHUNK,
    PNG_ERR_CRC_MISMATCH,
    PNG_ERR_NO_IHDR,
    PNG_ERR_NO_IDAT,
    PNG_ERR_NO_PLTE,        /* color type 3 with no PLTE chunk */
    PNG_ERR_UNSUPPORTED,    /* interlaced, 16-bit samples, or an invalid bitdepth/colortype combo */
    PNG_ERR_INFLATE,
    PNG_ERR_TRUNCATED,      /* inflated data shorter than width/height/bitdepth imply */
    PNG_ERR_OOM,
    PNG_ERR_MISUSE,         /* png_stream_* called with a row count/width that
                             * doesn't match the stream's declared dimensions */
} png_result_t;

/* Short human-readable form of a png_result_t (like render_result_str). */
const char *png_result_str(png_result_t r);

/* On success, `*out` is an owned, tightly-packed canvas_t (free with
 * canvas_free). */
png_result_t png_decode(const uint8_t *data, size_t len, canvas_t *out);
png_result_t png_decode_file(const char *path, canvas_t *out);

png_result_t png_encode_to_file(const char *path, const canvas_t *img);

/* Same format - one valid zlib stream - but scanlines are deflated in
 * independent row chunks, optionally on `nthreads` threads (<= 1 runs
 * inline). Each chunk's buffer is freed once compressed, so only a slice
 * of the raw image is resident. `level` is the zlib level (1-9). The bytes
 * depend on `nthreads`; the pixels don't. */
png_result_t png_encode_to_file_ex(const char *path, const canvas_t *img, int level, int nthreads);

/*
 * Incremental png_encode_to_file_ex for an image produced in horizontal
 * bands: the output is byte-identical to encoding it whole, and only a
 * <= 32 KB dictionary tail is kept between bands.
 *
 * png_stream_begin, then png_stream_write_rows per band (top to bottom,
 * no gaps or overlap, `height` rows in total), then png_stream_finish
 * once.
 */
typedef struct png_stream png_stream_t;

/* Opens `path` and writes IHDR, committing width and height. NULL on
 * failure; nothing else to call then. */
png_stream_t *png_stream_begin(const char *path, uint32_t width, uint32_t height, int level, int nthreads);

/* Writes the next `band->height` rows; `band->width` must match. Height 0
 * is a no-op. After an error every later call returns that error, so
 * checking png_stream_finish alone is enough. */
png_result_t png_stream_write_rows(png_stream_t *st, const canvas_t *band);

/* Ends the zlib stream and writes IEND, then frees `st`. Call once, after
 * all rows. Returns an earlier error, PNG_ERR_MISUSE if the row count was
 * wrong, or an I/O failure from the final flush. */
png_result_t png_stream_finish(png_stream_t *st);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_PNG_IMAGE_H */
