# Source layout

| Path | Contents |
|---|---|
| `cli/main.c`, `map.c`, `bake.c`, `iso.c` | The `prism` command: `map` a whole world, `bake` (the default) one region into tiles, `iso` an area into a picture |
| `cli/serve.c`, `app/` | `serve` (what no arguments do): the app, a local web server with its page (`app/`) and the viewer built in (`scripts/embed.sh`), which runs `prism map` for the world picked |
| `cli/info.c` | `info`: what a world's `level.dat` says (name, version, spawn), for the page in the browser; `serve` reads it the same way |
| `app/web.js`, `web/` | The app with no server: prism built for the browser (`make web`) run a command at a time in a worker (`prism-worker.js`), `prism map --plan` and `--index` around the bakes, the maps in the browser's storage served by `sw.js` |
| `viewer/site.html`, `app/export.js`, `scripts/demo.sh` | A map as a site of its own: `site.html` around the viewer, filled in by the app's Export (a zip, read over HTTP from either backend; it starts at the view `viewer/index.html` last kept in localStorage under "prism-view <map path>") or by `demo.sh` (the demo: a box baked with `prism map --clip`, which `window_load` empties the rest of before the light) |
| `cli/overview.c`, `app/picker.js` | The area picker: `prism overview` draws a region with isodraw a band of chunk rows at a time (memory), served by `prism serve` (`api/regions`, `api/overview`, kept under `<data>/overviews/`) or run in a worker (`web.js`, kept in the browser's storage); the picker reads a drag as the blocks the pixels show (the heights picture, `iso_image_heights`), lights up what the area takes, and bakes it as `prism map --clip` |
| `cli/win/` | The Windows build's manifest (UTF-8 paths) |
| `src/plat.h`, `src/plat/plat.c` | What differs between Windows and POSIX: processes, file renames and deletes, CPU and memory counts |
| `cli/skinlist.c` | The skins a world's player heads name, for `scripts/fetch_skins.sh` |
| `src/prism.h` | What the modules share, grouped by the file that defines it |
| `src/world/` | `states.c`, `atlas.c`: block states from the jar's models and tags, the texture atlas; `world.c`, `entities.c`: the voxel grid, biomes, light flood fill, banner, head and pot textures; `tint.c`: biome colours from the jar's colormaps and biome JSON; `window.c`: opening the jar once, loading the world window by window |
| `src/light/` | `light.c`: baked light per corner; `bounce.c`: bounced sunlight |
| `src/mesh/` | `mesh.c`: blocks to quads at levels 0 and 1; `coarse.c`: level 2 cells |
| `src/tiles/` | `tiles.c`: tile encoding and writing; `solid.c`: what a walker bumps into; `glb.c`: a glTF export of level 0 |
| `src/iso/` | `isodraw.c`: level 0 drawn on the CPU into an isometric picture, window by window, turned by any yaw about the area's middle |
| `scripts/` | `fetch_skins.sh`, and the ones below |
| `viewer/index.html`, `embed.html` | The viewer as a page; an example of one in a page |
| `viewer/js/viewer.js` | `createViewer`: the page element, renderer and camera, then each module in turn |
| `viewer/js/sun.js`, `sky.js`, `air.js` | The sun and moon; the sky dome and stars; haze and light shafts |
| `viewer/js/materials.js`, `shadows.js` | How the tiles take the light; the live shadow maps |
| `viewer/js/effects.js`, `frame.js` | Post-processing (FX); drawing a frame |
| `viewer/js/map.js`, `tile.js`, `tile-worker.js` | Loading tiles by distance; decoding them, in workers |
| `viewer/js/controls.js`, `walk.js`, `lens.js`, `photo.js` | Moving about and fly mode; walking; the camera panel; photos |
| `viewer/js/glb.js`, `diag.js` | A single region's `region.glb`; `?diag` and `?diag&bench` |
| `docs/` | These notes and the README's pictures |
| `libs/` | The C libraries: zip, NBT, region files, chunks, JSON, PNG, block models, canvas, the reader-writer lock (`libs/README.md`) |

# Scripts

## Checking the browser build

```bash
scripts/wasm_check.sh <client.jar> <world> <rx> <rz>
```

bakes a region with both the native and the browser build (`make wasm`)
and checks they come out the same: the atlas's pixels and every tile
decompressed. The compressed bytes may differ, each build having its own
zlib. Floating-point maths that the C library does goes through the `d*f`
wrappers, so glibc and musl give the same floats.

## The demo

```bash
scripts/demo.sh <client.jar> <world dir> <x0,z0,x1,z1,ymin> \
    [--out dir] [--title text] [--cam x,y,z --at x,y,z] [--sun deg]
    [--view 'cam=...&at=...']
```

bakes the box with `--clip` into `build-demo/` (or `--out`), with the
viewer and a page of its own (`viewer/site.html`, the one Export uses,
without its caption), the browser tab titled with the world's name. The
page starts at `--cam` looking at `--at`, or else from above the box's
north-west corner. `--view` is the whole start view instead, the viewer's
parameters as its address keeps them (copied from the address bar once the
view is framed: the lens and the time of day too). The demo linked from
the README, and its picture (`docs/demo.webp`), are

```bash
scripts/demo.sh 26.2.jar "New World" 1267,1003,1315,1045,35 \
    --view 'cam=1480.5,172,863.8&at=1296.1,65.4,1019.9&sun=252.5&fov=10.94&fstop=22&mini=1&ev=0.1&wb=6700'
```

## The GitHub Pages site

```bash
scripts/pages.sh
git push -f origin gh-pages
```

puts `web/landing.html` at the top, the app (`build-web/`, from `make web`)
under `app/` and the demo (`build-demo/`) under `demo/`, as the one commit
of the `gh-pages` branch: each run replaces the last, so old builds don't
pile up in the history. The app and the demo are in folders of their own
so the app's service worker never handles the demo's requests.
