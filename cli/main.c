/*
 * prism: a Minecraft Java world as a 3D map (map, a whole world, see map.c;
 * bake, one region, see bake.c) or as an isometric picture (iso, see iso.c).
 *
 *   prism map <client.jar> <world or region dir> <out dir> [--threads n]
 *   prism [bake] <client.jar> <world or region dir> <rx> <rz> <out dir> [...]
 *   prism iso <client.jar> <world or region dir> <x0> <z0> <x1> <z1> <out.png> [...]
 */
#include "cli.h"

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "map") == 0) return cmd_map(argc - 1, argv + 1);
    if (argc > 1 && strcmp(argv[1], "iso") == 0) return cmd_iso(argc - 1, argv + 1);
    if (argc > 1 && strcmp(argv[1], "bake") == 0) return cmd_bake(argc - 1, argv + 1);
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("usage: prism map <client.jar> <world or region dir> <out dir> [--threads n]\n"
               "       prism [bake] <client.jar> <world or region dir> <rx> <rz> <out dir> [threads] [glb]\n"
               "       prism iso <client.jar> <world or region dir> <x0> <z0> <x1> <z1> <out.png> [--sun deg] [--scale px]\n"
               "                 [--ss n] [--ymin y] [--threads n] [--noshadow]\n");
        return 0;
    }
    return cmd_bake(argc, argv);
}
