// FABLE-8 Pocket prototype. A bounded native PICO-8 subset, not a full emulator.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct pico8 pico8_t;
typedef struct {
    size_t lua_bytes, lua_peak, lua_limit, machine_bytes;
    uint32_t frames;
    unsigned update_hz;
} pico8_stats_t;

// Allocations are lazy: no machine/Lua RAM exists before create or after destroy.
// lua_limit bounds Lua's allocator, including compilation. Machine RAM is separate.
pico8_t *pico8_create(size_t lua_limit);
void pico8_destroy(pico8_t *p);
// A failed load/frame leaves the error available; destroy/recreate before another load.
bool pico8_load(pico8_t *p, const char *path);
bool pico8_frame(pico8_t *p, uint8_t held_buttons);
const char *pico8_error(const pico8_t *p);
void pico8_stats(const pico8_t *p, pico8_stats_t *out);
// Converts just one scanline. Caller needs 256 B, not an RGB framebuffer.
void pico8_scanline(const pico8_t *p, unsigned y, uint16_t rgb565[128]);
uint8_t pico8_pixel(const pico8_t *p, unsigned x, unsigned y);
#ifdef __cplusplus
}
#endif
