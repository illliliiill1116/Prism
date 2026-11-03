/*
 * statetint.c - colours for tinted faces that come from the block's own
 * state rather than the biome (redstone_wire's power). Keyed by block
 * name; biome colours are the renderer's job.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

static int prop_int(const blockmodel_property_t *props, size_t prop_count,
                    const char *name, int fallback) {
    size_t nlen = strlen(name);
    for (size_t i = 0; i < prop_count; i++) {
        if (props[i].name_len == nlen && memcmp(props[i].name, name, nlen) == 0) {
            /* value is a borrowed, not-necessarily-terminated view */
            int v = 0;
            for (size_t j = 0; j < props[i].value_len; j++) {
                char c = props[i].value[j];
                if (c < '0' || c > '9') return fallback;
                v = v * 10 + (c - '0');
            }
            return props[i].value_len ? v : fallback;
        }
    }
    return fallback;
}

static uint8_t clampf(double x) {
    if (x < 0.0) x = 0.0;
    if (x > 1.0) x = 1.0;
    return (uint8_t)(x * 255.0 + 0.5);
}

/* RedStoneWireBlock.getColorForPower: a red gradient from near-black at
 * power 0 to bright red at 15. Matches the game's float math. */
static void redstone_power_color(int power, uint8_t out[3]) {
    if (power < 0) power = 0;
    if (power > 15) power = 15;
    double f = power / 15.0;
    out[0] = clampf(f * 0.6 + (power > 0 ? 0.4 : 0.3));
    out[1] = clampf(f * f * 0.7 - 0.5);
    out[2] = clampf(f * f * 0.6 - 0.7);
}

int bm_state_tint(const char *block_name, size_t name_len,
                  const blockmodel_property_t *props, size_t prop_count,
                  uint8_t out_rgb[3]) {
    if (name_len == sizeof("minecraft:redstone_wire") - 1 &&
        memcmp(block_name, "minecraft:redstone_wire", name_len) == 0) {
        redstone_power_color(prop_int(props, prop_count, "power", 0), out_rgb);
        return 1;
    }
    return 0;
}
