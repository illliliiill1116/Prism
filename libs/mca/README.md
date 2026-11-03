# mca

Reads Anvil region files (`.mca`), on top of [../nbt](../nbt): this library
only finds a chunk's compressed data and inflates it, and `nbt_parse()` does
the NBT parsing. Depends on zlib.

## Format

Per the "Region file format" article on minecraft.wiki. A region covers 32x32
chunks:

```
byte 0    .. 4095 : location table  (1024 x 4 bytes)
byte 4096 .. 8191 : timestamp table (1024 x 4 bytes)
byte 8192 ..      : chunk payloads, aligned to 4096-byte sectors
```

- **Location table**: entry `(x&31) + (z&31)*32` is a 4-byte big-endian value;
  the high 3 bytes are the sector offset and the low byte the sector count.
  Both zero means the chunk was never generated (`mca_chunk_present()`).
- **Timestamp table**: same index, the last save time in epoch seconds, in
  `mca_chunk_loc_t.timestamp`.
- **Chunk payload**, at `sector_offset*4096`:

```
u32 length          (big-endian, includes the compression byte)
u8  compression     (1=gzip, 2=zlib, 3=uncompressed, 4=LZ4, 0x80=data in an external .mcc file)
u8  data[length-1]
```

gzip, zlib and uncompressed are supported. An external `.mcc` returns
`MCA_ERR_EXTERNAL` and LZ4 returns `MCA_ERR_BAD_COMPRESSION`; ordinary saves
rarely contain either.

## Design

- The file is opened with a read-only mmap, so only the sectors used are read
  into memory.
- Each chunk is inflated into a freshly allocated buffer before `nbt_parse()`
  (which rewrites its input in place), so the mapping is never modified and a
  region can be parsed in any order, any number of times.

## API ([include/mca.h](include/mca.h))

```c
mca_region_t region;
mca_region_open("r.0.0.mca", &region);

mca_chunk_loc_t loc;
mca_region_get_loc(&region, local_x, local_z, &loc);
if (mca_chunk_present(&loc)) {
    nbt_pool_t pool; nbt_tag_t *root;
    mca_region_parse_chunk(&region, local_x, local_z, &pool, &root);
    nbt_pool_free(&pool);   // the pool owns the inflated buffer
}

mca_region_close(&region);
```

## Build / tests

```sh
make          # build/libmca.a (builds ../nbt if needed)
make test     # tests against synthesized .mca files (test/synth_region.c)
```
