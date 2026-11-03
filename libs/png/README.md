# png

A minimal PNG decoder and encoder: enough to read Minecraft's textures and to
write rendered images. Reads and writes [../canvas](../canvas)'s `canvas_t`;
depends only on zlib.

## Supported

The scope follows all 1269 PNGs under `textures/block/` in the 26.2 client
jar:

| bit depth | colour type | count |
|-----------|-------------|-------|
| 8 | 3 (palette) | 677 |
| 4 | 3 (palette) | 339 |
| 8 | 6 (RGBA) | 116 |
| 2 | 3 (palette) | 60 |
| 8 | 2 (RGB) | 37 |
| 8 | 4 (grey+alpha) | 21 |
| 8 | 0 (grey) | 18 |
| 1 | 0 (grey) | 1 |

- Decoding: colour types 0/2/3/4/6, bit depths 1/2/4/8, PLTE + tRNS, multiple
  IDATs, and a CRC check on every chunk.
- 16-bit and interlaced (Adam7) images return `PNG_ERR_UNSUPPORTED` rather
  than a guess.
- Encoding: always 8-bit RGBA with filter None, reading rows through
  `stride`, so a subview encodes directly.
- Greyscale textures (`grass_block_top`, `water_still`) are tint masks.
  Tinting is the renderer's job; this only decodes the stored values.

## API ([include/png_image.h](include/png_image.h))

```c
canvas_t img;
png_decode_file("stone.png", &img);                 // RGBA8
png_encode_to_file_ex("out.png", &img, 3, nthreads); // deflate split by rows, in parallel
canvas_free(&img);

// a large image produced in bands: the output equals a one-shot encode,
// and only a 32 KB dictionary is kept between bands
png_stream_t *st = png_stream_begin("big.png", w, h, level, nthreads);
png_stream_write_rows(st, &band);   // top to bottom, band by band
png_stream_finish(st);
```

In a parallel encode each chunk is seeded with the tail of the previous one
as a preset dictionary (as pigz does), so the ratio stays close to a
single-threaded encode. The output bytes depend on the thread count; the
pixels don't.

## Tests

`test/fixtures/*.png` were generated independently with Python's standard
library (`zlib` + `struct`) and cover every combination in the table above,
plus an image that cycles through all five filter types. The tests therefore
check the decoder against the specification, not against this library's own
encoder.

```sh
make          # build/libpng_min.a
make test
```
