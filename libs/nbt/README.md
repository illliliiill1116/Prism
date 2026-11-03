# nbt

A zero-copy, low-allocation NBT parser. Read-only; depends on zlib.

## Format

Per the NBT format article on minecraft.wiki and Notch's original
specification (the hello world vector in `test/test_nbt.c` comes from it).
All numbers are big-endian.

### Tag types

| id | name | payload |
|----|------|---------|
| 0  | TAG_End | (no payload; ends a compound) |
| 1  | TAG_Byte | 1 byte, signed |
| 2  | TAG_Short | 2 bytes, big-endian, signed |
| 3  | TAG_Int | 4 bytes, big-endian, signed |
| 4  | TAG_Long | 8 bytes, big-endian, signed |
| 5  | TAG_Float | 4 bytes, big-endian IEEE 754 |
| 6  | TAG_Double | 8 bytes, big-endian IEEE 754 |
| 7  | TAG_Byte_Array | `int32 length` + `length` bytes |
| 8  | TAG_String | `uint16 length` + `length` bytes (Modified UTF-8, **not** null-terminated) |
| 9  | TAG_List | `uint8 element_type` + `int32 length` + `length` unnamed payloads of that type |
| 10 | TAG_Compound | named tags until a TAG_End |
| 11 | TAG_Int_Array | `int32 length` + `length` int32s |
| 12 | TAG_Long_Array | `int32 length` + `length` int64s |

A named tag (anything but a list element) is:

```
u8  type
u16 name_length
u8  name[name_length]   (not null-terminated)
... payload ...
```

List elements have no type or name prefix, only consecutive payloads, since
the element type is in the list's header.

Compression: `level.dat` and player data are gzipped as a whole; region file
chunks use zlib (see [../mca](../mca)). The 1.20.2+ network format (a root
without a name) is not supported.

## Design

- Tags live in a growable flat array, `nbt_pool_t.tags`. A compound's or
  list's children are a range in a second flat array, `children`. Indices
  survive realloc and pointers don't, so `nbt_child_at()` turns an index into
  a pointer only when it is read.
- Each level collects its direct children in an inline 32-entry scratch array
  (spilling to the heap beyond that) and commits them to `children` once the
  level is parsed, because grandchildren are appended to the same array first.
- Strings and byte arrays point straight into the input buffer; int and long
  arrays are byte-swapped in place. **The input buffer is modified**: it must
  outlive the pool's use and cannot be parsed a second time.
- `NBT_MAX_DEPTH` (512) caps nesting so a corrupt file can't exhaust the
  stack.

## API ([include/nbt.h](include/nbt.h), [include/nbt_io.h](include/nbt_io.h))

```c
nbt_pool_t pool; nbt_tag_t *root;
nbt_io_load_file("level.dat", &pool, &root);   // gzip / zlib / raw detected by magic bytes
const nbt_tag_t *data = nbt_compound_getz(&pool, root, "Data");
nbt_pool_free(&pool);                          // the pool owns the inflated buffer; one free
```

## Build / tests

```sh
make          # build/libnbt.a
make test     # hello world vector + synthesized-world round trip (test/synth.c)
```

The default flags include `-march=native`; override `CFLAGS` to run on other
machines.
