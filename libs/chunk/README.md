# chunk

Decodes the paletted containers in 1.18+ chunk sections into a palette index
per cell. Works directly on the tag tree from [../nbt](../nbt) (loaded with
`nbt_io` or [../mca](../mca)).

## Paletted container

Per the "Chunk format" article on minecraft.wiki. A section's `block_states`
and `biomes` share one structure:

| container | cells | each cell is | minimum bits |
|-----------|-------|--------------|--------------|
| `block_states` | 4096 (16x16x16) | one block | 4 |
| `biomes` | 64 (4x4x4) | one 4x4x4 biome cell | 0 |

```
TAG_Compound ("block_states" / "biomes")
  TAG_List "palette"
  TAG_Long_Array "data"    -- absent when the palette has one entry; every cell is index 0
```

- `block_states` palette entries are compounds: `"Name"`, and `"Properties"`
  when present. `biomes` palette entries are strings.
- Each value in `data` takes `bits = max(minimum, ceil(log2(palette length)))`
  bits. Since 1.16 values never span two longs: each long holds
  `floor(64/bits)` values and the remaining bits are padding. The older
  spanning layout is not supported.

## API ([include/chunk.h](include/chunk.h))

```c
uint16_t indices[CHUNK_BLOCKS_PER_SECTION];
const nbt_tag_t *palette;
chunk_decode_block_states(&pool, section, indices, &palette);

const char *name; uint16_t len;
chunk_palette_entry_name(&pool, palette, indices[i], &name, &len);   // points into the pool's buffer
const nbt_tag_t *props;
chunk_palette_entry_properties(&pool, palette, indices[i], &props);   // NULL when there are none
```

`chunk_decode_biomes()` is the biome version; both wrap
`chunk_decode_paletted_container()`. Decoding only reads the tag tree, so a
chunk can be decoded any number of times. Mapping names to colours or models
is up to the caller.

## Build / tests

```sh
make          # build/libchunk.a (builds ../nbt if needed)
make test     # exact round trips of synthesized sections (test/synth_section.c)
```
