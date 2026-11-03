# Embedding the viewer

## In a page of your own

The viewer fills whatever element it is given and follows its size, so it
can sit in a page like an image or a video:

```html
<script type="importmap">
{ "imports": {
    "three": "https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.module.js",
    "three/addons/": "https://cdn.jsdelivr.net/npm/three@0.160.0/examples/jsm/"
} }
</script>
<div id="world" style="height: 480px"></div>
<script type="module">
import { createViewer } from "./js/viewer.js";
const viewer = createViewer(document.getElementById("world"), { map: "mymap", sun: 100 });
</script>
```

`viewer/embed.html` is a complete example. The options are the [URL
parameters](../README.md#url-parameters) (`cam` and `at` as `[x, y, z]` or `"x,y,z"`), plus:

| Option | Meaning |
|---|---|
| `keys` | `"focus"` (default): the keys work once the viewer is clicked or tabbed to; `"page"`: anywhere on the page |
| `ui` | `false`: start with the controls hidden |
| `onView(params)` | Called a little after the view stops changing, with its URL parameters |
| `onReady()` | Called once everything in view has loaded |
| `onPhoto(canvas)` | Called with a photo in place of downloading it |

`createViewer` returns an object with `setView({ cam, at })`, `setSun(deg)`,
`setFly(on)`, `setFx(on)`, `setFullscreen(on)`, `photo(scale)`, `view()`,
`ready`, `destroy()` and `three` (the renderer, scene, camera and controls).
Several viewers can share a page; each is a WebGL context of its own until
`destroy()`. The page needs the import map above and the viewer's folder
(`js/` and `style.css`, which it adds to the page itself).

An `<iframe src="index.html?map=mymap" allow="fullscreen">` works too.

## From another site

The map and the viewer can be on a different site from the page (another
domain, or a CDN). Then that server must send `Access-Control-Allow-Origin`
(`*`, or the page's origin) with the map's files: `map.json`, `index.json`,
`atlas.png` and `tiles.bin`, and with the viewer's `js/` and `style.css` if
those are elsewhere too. Nothing more is needed: the viewer's Range requests
are plain `bytes=a-b` ones, which the browser sends without a preflight. For
example, in nginx:

```nginx
location /maps/ { add_header Access-Control-Allow-Origin *; }
```

Without the header the viewer shows "map load failed: ... must send
Access-Control-Allow-Origin". With the viewer's `js/` on another site, its
tile workers are started from a small `blob:` module of the page's own, as
browsers allow only same-site worker scripts. A page with a
Content-Security-Policy must then allow `worker-src blob:`. The iframe
needs none of this, since everything in it comes from one site.
