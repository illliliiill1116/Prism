# The baked map

Per region, `r.<x>.<z>/` holds:

| File | Contents |
|---|---|
| `index.json` | The format, the tile list with quad counts, and where each tile is in `tiles.bin` |
| `atlas.png` | Texture atlas |
| `tiles.bin` | Every tile of the region |

`tiles.bin` starts with all the coarse tiles in one pack. Then, per tile,
come the quads only level 0 has, the quads levels 0 and 1 share, and the
quads only level 1 has. Either level is one run of bytes, which the viewer
reads with an HTTP Range request. Getting closer downloads only the level 0
remainder, since the shared part is already there. Each part is gzipped on
its own. The format is documented where it is written (`tile_payload` and
`write_tiles` in `src/tiles/tiles.c`) and decoded in `viewer/js/tile.js`.

`map.json` and every `index.json` carry the format number (`"format"`).
The viewer refuses a map in a format it does not read and asks for a
rebake.

`map.json` also gives the world's water surface (`"sea"`, the height of
the top water block that most of its water columns have). The ground mist
lies on it and the view starts just above it, so a map whose sea is not at
the game's usual height (y 62) is not buried in mist. A map without it
(baked before) is taken to be at 62.

Caves are left out, as BlueMap does: unlit spaces below y 55, 7 under the
usual sea. Where a region's water is lower, the cut follows it down (never
up), so the closed rooms of a town built on an old superflat world at y 5
are kept.
