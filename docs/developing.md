# Source layout

| Path | Contents |
|---|---|
| `cli/main.c`, `map.c`, `bake.c`, `iso.c` | The `prism` command: `map` a whole world, `bake` (the default) one region into tiles, `iso` an area into a picture |
| `cli/skinlist.c` | The skins a world's player heads name, for `scripts/fetch_skins.sh` |
| `src/prism.h` | What the modules share, grouped by the file that defines it |
| `src/world/` | `states.c`, `atlas.c`: block states from the jar's models and tags, the texture atlas; `world.c`, `entities.c`: the voxel grid, biomes, light flood fill, banner, head and pot textures; `tint.c`: biome colours from the jar's colormaps and biome JSON; `window.c`: opening the jar once, loading the world window by window |
| `src/light/` | `light.c`: baked light per corner; `bounce.c`: bounced sunlight |
| `src/mesh/` | `mesh.c`: blocks to quads at levels 0 and 1; `coarse.c`: level 2 cells |
| `src/tiles/` | `tiles.c`: tile encoding and writing; `glb.c`: a glTF export of level 0 |
| `src/iso/` | `isodraw.c`: level 0 drawn on the CPU into an isometric picture, window by window |
| `scripts/` | `fetch_skins.sh` |
| `viewer/index.html`, `embed.html` | The viewer as a page; an example of one in a page |
| `viewer/js/viewer.js` | `createViewer`: the page element, renderer and camera, then each module in turn |
| `viewer/js/sun.js`, `sky.js`, `air.js` | The sun and moon; the sky dome and stars; haze and light shafts |
| `viewer/js/materials.js`, `shadows.js` | How the tiles take the light; the live shadow maps |
| `viewer/js/effects.js`, `frame.js` | Post-processing (FX); drawing a frame |
| `viewer/js/map.js`, `tile.js`, `tile-worker.js` | Loading tiles by distance; decoding them, in workers |
| `viewer/js/controls.js`, `lens.js`, `photo.js` | Moving about and fly mode; the camera panel; photos |
| `viewer/js/glb.js`, `diag.js` | A single region's `region.glb`; `?diag` and `?diag&bench` |
| `docs/` | These notes and the README's pictures |
| `libs/` | The C libraries: zip, NBT, region files, chunks, JSON, PNG, block models, canvas, the reader-writer lock (`libs/README.md`) |
