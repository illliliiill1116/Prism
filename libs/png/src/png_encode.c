#include "png_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <zlib.h>

const char *png_result_str(png_result_t r) {
    switch (r) {
    case PNG_OK:                return "ok";
    case PNG_ERR_IO:            return "file read/write failed";
    case PNG_ERR_BAD_SIGNATURE: return "not a PNG file";
    case PNG_ERR_BAD_CHUNK:     return "malformed PNG chunk";
    case PNG_ERR_CRC_MISMATCH:  return "PNG chunk CRC mismatch";
    case PNG_ERR_NO_IHDR:       return "PNG has no IHDR chunk";
    case PNG_ERR_NO_IDAT:       return "PNG has no image data";
    case PNG_ERR_NO_PLTE:       return "paletted PNG with no palette";
    case PNG_ERR_UNSUPPORTED:   return "PNG form this reader does not support";
    case PNG_ERR_INFLATE:       return "zlib inflate/deflate failed";
    case PNG_ERR_TRUNCATED:     return "PNG image data ends early";
    case PNG_ERR_OOM:           return "out of memory";
    case PNG_ERR_MISUSE:        return "row count or width disagrees with the stream's dimensions";
    }
    return "unknown error";
}

static void put_u32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static png_result_t write_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t len_buf[4];
    put_u32be(len_buf, len);
    if (fwrite(len_buf, 1, 4, f) != 4) return PNG_ERR_IO;

    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)type, 4);
    if (len > 0) crc = crc32(crc, data, len);

    if (fwrite(type, 1, 4, f) != 4) return PNG_ERR_IO;
    if (len > 0 && fwrite(data, 1, len, f) != len) return PNG_ERR_IO;

    uint8_t crc_buf[4];
    put_u32be(crc_buf, (uint32_t)crc);
    if (fwrite(crc_buf, 1, 4, f) != 4) return PNG_ERR_IO;
    return PNG_OK;
}

static const uint8_t kSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

/* One row chunk's deflated output. `raw_len` (its raw scanline bytes) is
 * kept to combine the chunks' adler32s into the stream's one trailer. */
typedef struct {
    uint8_t *data;
    size_t len;
    uLong adler;
    size_t raw_len;
} chunk_result_t;

typedef struct {
    const canvas_t *img;
    size_t row_bytes; /* PNG on-disk row size (width*4) - NOT necessarily img->stride */
    uint32_t rows_per_chunk;
    uint32_t nchunks;
    int level;
    uint32_t next_chunk; /* protected by mutex */
    pthread_mutex_t mutex;
    chunk_result_t *results;
    int failed;
    /* Streaming: `img` is one band, so no chunk ends the stream - every
     * chunk uses Z_FULL_FLUSH and png_stream_finish terminates it.
     * `prior_tail` seeds chunk 0's dictionary from the previous band
     * (NULL/0 for single-shot and a stream's first band). */
    int is_stream;
    const uint8_t *prior_tail;
    size_t prior_tail_len;
} deflate_job_t;

#define DICT_CAP 32768u

/* Fills the end of `dict[DICT_CAP]` with up to DICT_CAP raw bytes that
 * precede row `row0`; returns the length (content at dict[DICT_CAP - len
 * .. DICT_CAP)). Seeding each chunk with its predecessor's tail keeps the
 * ratio of a parallel deflate close to a serial one, as pigz does. */
static size_t build_dict_tail(const canvas_t *img, size_t row_bytes, uint32_t row0, uint8_t *dict) {
    size_t remaining = DICT_CAP;
    uint8_t *cursor = dict + DICT_CAP;
    int64_t r = (int64_t)row0 - 1;
    while (r >= 0 && remaining > 0) {
        size_t row_full = row_bytes + 1;
        size_t take = row_full < remaining ? row_full : remaining;
        cursor -= take;
        if (take <= row_bytes) {
            memcpy(cursor, canvas_row(img, (uint32_t)r) + (row_bytes - take), take);
        } else {
            cursor[0] = 0; /* filter type: None */
            memcpy(cursor + 1, canvas_row(img, (uint32_t)r), row_bytes);
        }
        remaining -= take;
        r--;
    }
    return DICT_CAP - remaining;
}

static void compress_one_chunk(deflate_job_t *job, uint32_t idx) {
    uint32_t row0 = idx * job->rows_per_chunk;
    uint32_t rows = job->rows_per_chunk;
    if (row0 + rows > job->img->height) rows = job->img->height - row0;
    int is_last = (!job->is_stream) && (idx == job->nchunks - 1);

    size_t raw_len = (job->row_bytes + 1) * rows;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) { job->failed = 1; return; }

    for (uint32_t r = 0; r < rows; r++) {
        uint8_t *dst = raw + (size_t)r * (job->row_bytes + 1);
        dst[0] = 0; /* filter type: None */
        memcpy(dst + 1, canvas_row(job->img, row0 + r), job->row_bytes);
    }

    uint8_t dict_buf[DICT_CAP];
    size_t dict_len = 0;
    if (idx > 0) {
        dict_len = build_dict_tail(job->img, job->row_bytes, row0, dict_buf);
    } else if (job->prior_tail_len > 0) {
        dict_len = job->prior_tail_len;
        memcpy(dict_buf + (DICT_CAP - dict_len), job->prior_tail, dict_len);
    }

    z_stream strm;
    memset(&strm, 0, sizeof strm);
    /* Raw deflate (negative windowBits). Every chunk but the last ends with
     * Z_FULL_FLUSH (byte-aligned, BFINAL=0) so the chunks concatenate into
     * one valid stream; only the last uses Z_FINISH. */
    if (deflateInit2(&strm, job->level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(raw);
        job->failed = 1;
        return;
    }
    if (dict_len > 0) deflateSetDictionary(&strm, dict_buf + (DICT_CAP - dict_len), (uInt)dict_len);

    uLong bound = deflateBound(&strm, (uLong)raw_len) + 64; /* +64: flush-marker slack */
    uint8_t *out = (uint8_t *)malloc(bound);
    if (!out) {
        deflateEnd(&strm);
        free(raw);
        job->failed = 1;
        return;
    }
    strm.next_in = raw;
    strm.avail_in = (uInt)raw_len;
    strm.next_out = out;
    strm.avail_out = (uInt)bound;
    int flush = is_last ? Z_FINISH : Z_FULL_FLUSH;
    int zr;
    do {
        zr = deflate(&strm, flush);
    } while (strm.avail_out > 0 && strm.avail_in > 0 && zr == Z_OK);
    size_t out_len = bound - strm.avail_out;
    deflateEnd(&strm);
    int ok = is_last ? (zr == Z_STREAM_END) : (zr == Z_OK && strm.avail_in == 0);
    if (!ok) {
        free(raw);
        free(out);
        job->failed = 1;
        return;
    }
    uLong adler = adler32(adler32(0L, Z_NULL, 0), raw, (uInt)raw_len);
    free(raw);

    job->results[idx].data = out;
    job->results[idx].len = out_len;
    job->results[idx].adler = adler;
    job->results[idx].raw_len = raw_len;
}

static void *deflate_worker(void *arg) {
    deflate_job_t *job = (deflate_job_t *)arg;
    for (;;) {
        pthread_mutex_lock(&job->mutex);
        uint32_t idx = job->next_chunk++;
        pthread_mutex_unlock(&job->mutex);
        if (idx >= job->nchunks) break;
        compress_one_chunk(job, idx);
    }
    return NULL;
}

png_result_t png_encode_to_file_ex(const char *path, const canvas_t *img, int level, int nthreads) {
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    if (nthreads < 1) nthreads = 1;

    size_t row_bytes = (size_t)img->width * 4;
    uint32_t height = img->height;
    if (height == 0) height = 1; /* keep chunking math well-defined; loop below still no-ops for a truly empty image */

    /* About 8 chunks per thread: balances load and bounds the resident
     * raw scanlines without per-chunk overhead dominating. */
    uint32_t target_chunks = (uint32_t)nthreads * 8;
    if (target_chunks < 1) target_chunks = 1;
    uint32_t rows_per_chunk = height / target_chunks;
    if (rows_per_chunk < 1) rows_per_chunk = 1;
    uint32_t nchunks = (height + rows_per_chunk - 1) / rows_per_chunk;

    deflate_job_t job;
    job.img = img;
    job.row_bytes = row_bytes;
    job.rows_per_chunk = rows_per_chunk;
    job.nchunks = nchunks;
    job.level = level;
    job.next_chunk = 0;
    job.failed = 0;
    job.is_stream = 0;
    job.prior_tail = NULL;
    job.prior_tail_len = 0;
    job.results = (chunk_result_t *)calloc(nchunks, sizeof(chunk_result_t));
    if (!job.results) return PNG_ERR_OOM;
    pthread_mutex_init(&job.mutex, NULL);

    int nworkers = nthreads;
    if ((uint32_t)nworkers > nchunks) nworkers = (int)nchunks;

    if (nworkers <= 1) {
        for (uint32_t i = 0; i < nchunks && !job.failed; i++) compress_one_chunk(&job, i);
    } else {
        pthread_t *threads = (pthread_t *)malloc((size_t)(nworkers - 1) * sizeof(pthread_t));
        if (!threads) { free(job.results); pthread_mutex_destroy(&job.mutex); return PNG_ERR_OOM; }
        for (int t = 0; t < nworkers - 1; t++) pthread_create(&threads[t], NULL, deflate_worker, &job);
        deflate_worker(&job); /* main thread joins the pool as a worker too */
        for (int t = 0; t < nworkers - 1; t++) pthread_join(threads[t], NULL);
        free(threads);
    }
    pthread_mutex_destroy(&job.mutex);

    png_result_t result = PNG_OK;
    if (job.failed) result = PNG_ERR_OOM;

    if (result == PNG_OK) {
        FILE *f = fopen(path, "wb");
        if (!f) result = PNG_ERR_IO;
        else {
            if (fwrite(kSignature, 1, 8, f) != 8) result = PNG_ERR_IO;

            if (result == PNG_OK) {
                uint8_t ihdr[13];
                put_u32be(ihdr, img->width);
                put_u32be(ihdr + 4, img->height);
                ihdr[8] = 8;  /* bit depth */
                ihdr[9] = 6;  /* color type: RGBA */
                ihdr[10] = 0; /* compression */
                ihdr[11] = 0; /* filter */
                ihdr[12] = 0; /* interlace */
                result = write_chunk(f, "IHDR", ihdr, 13);
            }

            /* The zlib header, as its own small leading IDAT. */
            if (result == PNG_OK) {
                uint8_t zheader[2] = {0x78, 0x9C}; /* CM=8/CINFO=7, default FLEVEL, no preset dict */
                result = write_chunk(f, "IDAT", zheader, 2);
            }

            uLong running_adler = adler32(0L, Z_NULL, 0);
            for (uint32_t i = 0; i < nchunks && result == PNG_OK; i++) {
                chunk_result_t *cr = &job.results[i];
                if (cr->len > 0) result = write_chunk(f, "IDAT", cr->data, (uint32_t)cr->len);
                running_adler = adler32_combine(running_adler, cr->adler, (long)cr->raw_len);
            }

            if (result == PNG_OK) {
                uint8_t trailer[4];
                put_u32be(trailer, (uint32_t)running_adler);
                result = write_chunk(f, "IDAT", trailer, 4);
            }

            if (result == PNG_OK) result = write_chunk(f, "IEND", NULL, 0);

            if (f) fclose(f);
        }
    }

    for (uint32_t i = 0; i < nchunks; i++) free(job.results[i].data);
    free(job.results);
    return result;
}

struct png_stream {
    FILE *f;
    uint32_t width, height;
    size_t row_bytes;
    int level, nthreads;
    uLong running_adler;
    uint32_t rows_written;
    /* The last band's <= 32 KB raw tail, right-aligned like
     * build_dict_tail's output: the next band's dictionary seed. */
    uint8_t tail[DICT_CAP];
    size_t tail_len;
    png_result_t err;
};

png_stream_t *png_stream_begin(const char *path, uint32_t width, uint32_t height, int level, int nthreads) {
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    if (nthreads < 1) nthreads = 1;

    png_stream_t *st = (png_stream_t *)calloc(1, sizeof(png_stream_t));
    if (!st) return NULL;
    st->width = width;
    st->height = height;
    st->row_bytes = (size_t)width * 4;
    st->level = level;
    st->nthreads = nthreads;
    st->running_adler = adler32(0L, Z_NULL, 0);
    st->err = PNG_OK;

    st->f = fopen(path, "wb");
    if (!st->f) { free(st); return NULL; }

    if (fwrite(kSignature, 1, 8, st->f) != 8) st->err = PNG_ERR_IO;

    if (st->err == PNG_OK) {
        uint8_t ihdr[13];
        put_u32be(ihdr, width);
        put_u32be(ihdr + 4, height);
        ihdr[8] = 8;  /* bit depth */
        ihdr[9] = 6;  /* color type: RGBA */
        ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
        st->err = write_chunk(st->f, "IHDR", ihdr, 13);
    }

    if (st->err == PNG_OK) {
        uint8_t zheader[2] = {0x78, 0x9C};
        st->err = write_chunk(st->f, "IDAT", zheader, 2);
    }

    if (st->err != PNG_OK) { fclose(st->f); free(st); return NULL; }
    return st;
}

png_result_t png_stream_write_rows(png_stream_t *st, const canvas_t *band) {
    if (st->err != PNG_OK) return st->err;
    if (band->height == 0) return PNG_OK;
    if (band->width != st->width || st->rows_written + band->height > st->height) {
        st->err = PNG_ERR_MISUSE;
        return st->err;
    }

    uint32_t height = band->height;
    uint32_t target_chunks = (uint32_t)st->nthreads * 8;
    if (target_chunks < 1) target_chunks = 1;
    uint32_t rows_per_chunk = height / target_chunks;
    if (rows_per_chunk < 1) rows_per_chunk = 1;
    uint32_t nchunks = (height + rows_per_chunk - 1) / rows_per_chunk;

    deflate_job_t job;
    job.img = band;
    job.row_bytes = st->row_bytes;
    job.rows_per_chunk = rows_per_chunk;
    job.nchunks = nchunks;
    job.level = st->level;
    job.next_chunk = 0;
    job.failed = 0;
    job.is_stream = 1;
    job.prior_tail = st->tail + (DICT_CAP - st->tail_len);
    job.prior_tail_len = st->tail_len;
    job.results = (chunk_result_t *)calloc(nchunks, sizeof(chunk_result_t));
    if (!job.results) { st->err = PNG_ERR_OOM; return st->err; }
    pthread_mutex_init(&job.mutex, NULL);

    int nworkers = st->nthreads;
    if ((uint32_t)nworkers > nchunks) nworkers = (int)nchunks;

    if (nworkers <= 1) {
        for (uint32_t i = 0; i < nchunks && !job.failed; i++) compress_one_chunk(&job, i);
    } else {
        pthread_t *threads = (pthread_t *)malloc((size_t)(nworkers - 1) * sizeof(pthread_t));
        if (!threads) {
            job.failed = 1;
        } else {
            for (int t = 0; t < nworkers - 1; t++) pthread_create(&threads[t], NULL, deflate_worker, &job);
            deflate_worker(&job);
            for (int t = 0; t < nworkers - 1; t++) pthread_join(threads[t], NULL);
            free(threads);
        }
    }
    pthread_mutex_destroy(&job.mutex);

    if (job.failed) {
        for (uint32_t i = 0; i < nchunks; i++) free(job.results[i].data);
        free(job.results);
        st->err = PNG_ERR_OOM;
        return st->err;
    }

    for (uint32_t i = 0; i < nchunks; i++) {
        chunk_result_t *cr = &job.results[i];
        if (cr->len > 0 && st->err == PNG_OK) st->err = write_chunk(st->f, "IDAT", cr->data, (uint32_t)cr->len);
        st->running_adler = adler32_combine(st->running_adler, cr->adler, (long)cr->raw_len);
        free(cr->data);
    }
    free(job.results);

    st->tail_len = build_dict_tail(band, st->row_bytes, band->height, st->tail);
    st->rows_written += band->height;

    return st->err;
}

png_result_t png_stream_finish(png_stream_t *st) {
    if (!st) return PNG_ERR_IO;

    png_result_t result = st->err;
    if (result == PNG_OK && st->rows_written != st->height) result = PNG_ERR_MISUSE;

    if (result == PNG_OK) {
        /* Every band flushed with BFINAL=0; this empty Z_FINISH block ends
         * the stream. */
        z_stream strm;
        memset(&strm, 0, sizeof strm);
        if (deflateInit2(&strm, st->level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            result = PNG_ERR_OOM;
        } else {
            if (st->tail_len > 0)
                deflateSetDictionary(&strm, st->tail + (DICT_CAP - st->tail_len), (uInt)st->tail_len);
            uint8_t out[64];
            strm.next_in = NULL;
            strm.avail_in = 0;
            strm.next_out = out;
            strm.avail_out = sizeof(out);
            int zr = deflate(&strm, Z_FINISH);
            size_t out_len = sizeof(out) - strm.avail_out;
            deflateEnd(&strm);
            if (zr != Z_STREAM_END) result = PNG_ERR_OOM;
            else if (out_len > 0) result = write_chunk(st->f, "IDAT", out, (uint32_t)out_len);
        }
    }

    if (result == PNG_OK) {
        uint8_t trailer[4];
        put_u32be(trailer, (uint32_t)st->running_adler);
        result = write_chunk(st->f, "IDAT", trailer, 4);
    }

    if (result == PNG_OK) result = write_chunk(st->f, "IEND", NULL, 0);

    if (st->f) fclose(st->f);
    free(st);
    return result;
}

png_result_t png_encode_to_file(const char *path, const canvas_t *img) {
    return png_encode_to_file_ex(path, img, Z_DEFAULT_COMPRESSION, 1);
}
