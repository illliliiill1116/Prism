# zip

A minimal read-only ZIP/JAR reader: open a client jar, find an entry by its
full path, and extract it to memory. Depends only on zlib.

## Supported

- Read-only. Compression methods 0 (stored) and 8 (deflate); anything else
  returns `ZIP_ERR_UNSUPPORTED_METHOD`.
- No ZIP64, multi-disk archives or encryption, none of which a client jar
  uses (the 26.2 jar has about 32,000 entries and is 39 MB).
- The file is mmapped. `zip_open` walks the central directory once, and
  entries point straight into the mapping (zero-copy).
- Lookups are a linear scan. Each distinct block, model and texture is looked
  up once, so this is not a bottleneck.
- ZIP stores raw DEFLATE data, so it is inflated with raw inflate
  (`windowBits = -15`).

## API ([include/zip_reader.h](include/zip_reader.h))

```c
zip_archive_t jar;
zip_open("/path/to/client.jar", &jar);

const zip_entry_t *e = zip_findz(&jar, "assets/minecraft/textures/block/stone.png");
uint8_t *buf; size_t len;
zip_extract(&jar, e, &buf, &len);   // the caller frees buf

zip_close(&jar);
```

## Tests

`test/fixtures/sample.zip` was made with Python's `zipfile` and covers stored
and deflated entries and a nested path.

```sh
make          # build/libzipreader.a
make test
```
