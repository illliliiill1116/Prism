/*
 * prism: a Minecraft Java world as a 3D map (map, a whole world, see map.c;
 * bake, one region, see bake.c) or as an isometric picture (iso, see iso.c).
 *
 *   prism map <client.jar> <world or region dir> <out dir> [--threads n]
 *   prism [bake] <client.jar> <world or region dir> <rx> <rz> <out dir> [...]
 *   prism iso <client.jar> <world or region dir> <x0> <z0> <x1> <z1> <out.png> [...]
 *   prism overview <client.jar> <world or region dir> <out dir> <rx> <rz>... [...]  (see overview.c)
 *   prism serve [...]  (the app in the browser, see serve.c; also what no arguments do)
 */
#include "cli.h"

int main(int argc, char **argv) {
#ifndef __EMSCRIPTEN__ /* the browser build is driven by its page, no server */
    if (argc == 1) return cmd_serve(argc, argv);
    if (strcmp(argv[1], "serve") == 0) return cmd_serve(argc - 1, argv + 1);
#endif
    if (argc > 1 && strcmp(argv[1], "map") == 0) return cmd_map(argc - 1, argv + 1);
    if (argc > 1 && strcmp(argv[1], "info") == 0) return cmd_info(argc - 1, argv + 1);
    if (argc > 1 && strcmp(argv[1], "iso") == 0) return cmd_iso(argc - 1, argv + 1);
    if (argc > 1 && strcmp(argv[1], "overview") == 0) return cmd_overview(argc - 1, argv + 1);
    if (argc > 1 && strcmp(argv[1], "bake") == 0) return cmd_bake(argc - 1, argv + 1);
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("usage: prism map <client.jar> <world or region dir> <out dir> [--threads n] [--near x,z,r] [--plan | --index]\n"
               "       prism [bake] <client.jar> <world or region dir> <rx> <rz> <out dir> [threads] [glb]\n"
               "       prism iso <client.jar> <world or region dir> <x0> <z0> <x1> <z1> <out.png> [--sun deg] [--scale px]\n"
               "                 [--ss n] [--ymin y] [--threads n] [--noshadow]\n"
               "       prism overview <client.jar> <world or region dir> <out dir> <rx> <rz> [<rx> <rz>...] [--scale px] [--ymin y]\n"
               "       prism info <world dir>\n"
               "       prism serve [--port n] [--mc dir] [--data dir] [--no-open] [--www dir]   (also: no arguments)\n");
        return 0;
    }
    return cmd_bake(argc, argv);
}
