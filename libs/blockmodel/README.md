# blockmodel

Resolves a block (its name plus blockstate properties) to what Minecraft
actually draws. Everything is read from the blockstates, models and textures
in the client jar; there are no hand-written lookup tables. Depends on
[../zip](../zip), [../mcjson](../mcjson), [../png](../png),
[../canvas](../canvas) and [../percpu_rwlock](../percpu_rwlock).

## Resolution chain

```
block state (minecraft:oak_stairs, facing=north,half=bottom,...)
  -> blockstates/oak_stairs.json   "variants" picks one by properties, or "multipart" combines several by "when"
  -> models/block/oak_stairs.json  "textures" fills texture variables, "parent" inherits upward
  -> models/block/stairs.json      "elements" defines the geometry (the non-empty one nearest the leaf)
  -> "#texture" variables resolve to a concrete reference, minecraft:block/oak_planks
  -> textures/block/oak_planks.png
```

## API ([include/blockmodel.h](include/blockmodel.h))

```c
blockmodel_registry_t *reg;
blockmodel_open("/path/to/client.jar", &reg);
blockmodel_set_shards(reg, nthreads);          // once, before any multithreaded use

// six flat face textures (the fast path for full cubes)
const block_faces_t *faces;
blockmodel_resolve(reg, name, len, &faces);                   // by name, first variant
blockmodel_resolve_state(reg, name, len, props, n, &out);     // variant chosen by properties

// real geometry: each element's box, each face's texture/uv/tint, the model's x/y rotation
const blockmodel_shape_t *shape;
blockmodel_resolve_shape(reg, name, len, props, n, &shape);

blockmodel_close(reg);
```

Results are owned by the registry and stay valid until `blockmodel_close`.
The caches are fixed-capacity, non-relocating arrays with a hash index,
guarded by `percpu_rwlock`: a lookup takes only its own shard's read lock,
and the expensive zip/JSON/PNG work happens outside the lock.

## Supported

- `variants` property matching, and `multipart` `when` clauses (AND, `OR`/`AND`
  arrays, `|` alternatives).
- Whole-model x/y rotation in multiples of 90°, including its effect on each
  face's texture orientation (`uvrot`).
- The newer texture form `{"sprite": ..., "force_translucent": ...}`.
- Tint colours that depend on the blockstate (redstone wire's `power`), see
  `statetint.c`; biome colours are handled in `../../src/world/tint.c`.
- Chests, shulker boxes, heads (the dragon's and piglin's too), banners,
  decorated pots, conduits, copper golem statues and end portals and
  gateways. The game draws these block entities with entity renderers and
  their models have no elements, so their geometry is synthesized from
  hard-coded cube tables in `entity_shapes.c`, the numbers as the game's
  model code has them. A posed part (a head turned in 22.5 degree steps,
  a statue's limbs) carries an affine `xf`, applied after the element's
  own rotation. What their block entity holds (a banner's patterns, a
  head's skin, a pot's sherds) is the caller's to read: it swaps in a
  texture of the same layout (`blockmodel_pot_texture` builds a pot's).
  The end portal's and gateway's starfield is a shader in the game; here
  it is one still of it, made into a texture.

## Not supported

- `uvlock`.
- Arbitrary per-element `rotation`: it is kept in the `rot_*` fields but not
  applied to the box.
- Default uvs for partial elements that omit `uv` (they fall back to
  `[0,0,16,16]`, which is only right for full cubes).
- Weighted random variants: the first one is always used.

## Build / tests

```sh
make                                    # build/libblockmodel.a
make test JAR=/path/to/client.jar       # needs a real jar (Mojang's assets can't be in the repo)
```
