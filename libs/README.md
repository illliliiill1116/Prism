# libs/ — shared conventions

Each library has its own README (canvas and percpu_rwlock are documented in
their headers). This file lists the rules that hold across all of them.

## Naming

- Types are `<lib>_<thing>_t`, from `typedef struct <lib>_<thing> { ... }`.
  No bare `struct` in a public signature.
- Functions are `<lib>_<verb>`, with the prefix matching the header (`zip_`,
  `nbt_`, `mca_`, `json_`, `png_`, `chunk_`, `blockmodel_`,
  `canvas_`, `percpu_rwlock_`).
- A `z` suffix takes a null-terminated string instead of a length
  (`zip_findz`, `nbt_compound_getz`, `json_object_getz`). These are `static inline` wrappers; the
  length-taking form is the real one, since names here usually point into
  mapped buffers and aren't null-terminated.

## Errors

- A function that can fail in distinguishable ways returns a
  `<lib>_result_t` by value: `<LIB>_OK = 0`, then `<LIB>_ERR_*`. Never
  errno or a bare `-1`.
- A function that can only fail one way returns `int` (1 = success, 0 =
  OOM) or `NULL`.
- `canvas_create()` returns a `canvas_t` by value; `.pixels == NULL` means
  the allocation failed.
- Destructors return `void`, except `png_stream_finish`, which ends a
  deferred pipeline and is the only place its errors surface. Read its
  result.
- `png_result_str()` exists because those codes reach a person; add a
  `*_str` only when a code starts being printed.

## Out-parameters

- A parameter the function writes and the caller doesn't pre-fill is named
  `out` or `out_<thing>`. Receivers (`reg`, `pool`, `st`, ...) are not.
- **Out-parameters are written only on success**; check the result first.
- Exceptions, where the caller must be able to use the storage after a
  failure:
  - `zip_open()` and `mca_region_open()` zero their caller-owned target
    before anything can fail, so `zip_close()`/`mca_region_close()` are
    safe after a failed open. Their parameter is named for the handle
    (`archive`, `region`), not `out_*`.
- `percpu_rwlock_init` doesn't zero first, but leaves `l->shards` NULL on
  failure, which `percpu_rwlock_destroy` checks.

## Lifetimes

- `*_open`/`*_close` for files (`zip`, `mca`, `blockmodel`), `*_init`/`*_free`
  for caller-owned structs (`nbt_pool`, `json_pool`), `*_create`/`*_free`
  when the library allocates (`canvas`), and
  `percpu_rwlock_init`/`percpu_rwlock_destroy`.
- **Every destructor is NULL-safe** (`f(NULL)` is a no-op), in `libs/`.
- Destructors reset what they free, so destroying twice is a no-op.

## Threading

Everything assumes one thread per object, except:

- `percpu_rwlock` — the sharded reader-writer lock on C11 atomics (no
  pthread locks) that the shared caches use.
- `blockmodel` — its registry is shared across render threads and locks
  its own caches; call `blockmodel_set_shards` before any worker starts.
- `png` — `png_encode_to_file_ex`/`png_stream_*` split the encode over
  `nthreads`. The output bytes depend on the thread count; the pixels
  don't.
