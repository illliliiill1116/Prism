# mcjson

A minimal JSON parser for Minecraft's blockstate and model files. No
dependencies.

## Design

- All values live in one growable flat array and are referenced by index
  (stable across realloc), the same approach as [../nbt](../nbt).
- An object's or array's children are a contiguous range in a second flat
  array. Children are collected in an inline small-vector and committed once
  the whole node is parsed, because grandchildren are appended first.
- Strings without escapes point straight into the input buffer; only strings
  with escapes are decoded into `string_arena`.

## Supported

The six standard JSON value types, string escapes including `\uXXXX`, and
numbers stored as `double`. No comments, no trailing commas, and no strict
UTF-8 validation.

## API ([include/json.h](include/json.h))

```c
json_pool_t pool;
json_pool_init(&pool);
json_value_t *root;
json_parse(&pool, text, text_len, &root);   // text must outlive the pool's use

json_value_t *variants = json_object_getz(&pool, root, "variants");
// variant keys are arbitrary property strings; iterate with json_object_key_at / json_object_value_at

json_pool_free(&pool);
```

## Tests

```sh
make          # build/libmcjson.a
make test
```
