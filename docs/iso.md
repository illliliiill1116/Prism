# The 2.5D picture

```bash
build/prism iso ~/.minecraft/versions/26.2/26.2.jar \
    ~/.minecraft/saves/MyWorld -300 -300 300 300 area.png --scale 4
```

draws the world from x, z = -300, -300 to 300, 300 (both corners inside)
as an isometric PNG on the CPU (each block's top a 32 x 16 diamond at the
default 16 px a block). It uses the
same mesh and baked light as the viewer, at one time of day: the far
shadows are worked out for that sun alone, not along its whole arc, the
near ones come from a shadow map along it, only the faces that show in the
picture are lit (it is drawn once to find them), and the viewer's
effects, ripples and wind are left out.

The area is loaded in windows of a region's size, each with the world
around it, so the light and the shadows falling into the area are the
world's. The ground is cut open at the area's edge and at `--ymin`; the cut
is lit flat, so its layers show (as are the sides of chunks never
generated). The picture is held in memory until it is written, cropped to
what was drawn: about 4 bytes a pixel of it. A 600 x 600 area takes about
7 s and 2.6 GB at 4 px a block (11 s and 3.3 GB at 16 px, a gigabyte of
it the picture), a 120 x 120 one under 2 s at 16 px; much of that is
loading the world around each window.

The output names the picture's corner in projected pixels (`sx = (x - z)
S`, `sy = (x + z) S / 2 + (320 - y) S + S / 2`, S pixels a block), so
pictures of neighbouring areas can be put together.

With `--yaw`, the area is turned about its middle before it is projected,
so the corner is in the turned projection and pictures put together this
way have to share the yaw and the area's middle.

## prism overview

```bash
build/prism overview <client.jar> <world> <out dir> <rx> <rz> [<rx> <rz>...] \
    [--scale px] [--ymin y] [--threads n]
```

draws whole regions the same way but quickly: no cast shadows, the least
bounced light, 2 pixels a block by default, about 2.5 s and 1.2 GB a region.
Each region gives `r.<x>.<z>.png`, `r.<x>.<z>.h.png` (the world y each
pixel shows, as `(y + 64) * 128 + 1` in red and green, so the picker knows
which block a pixel is) and `r.<x>.<z>.json`, the pictures' corner in
projected pixels and their size (`w` 0 and no pictures for an empty
region). Drawn in order of `rx + rz`, each at its corner, they make the
world without a seam: a region is drawn a quarter at a time, as iso draws
an area, with the world around it and not cut at its edges. The app's
area picker is made of them.
