// Original native subset; Lua 5.4.8 is vendored separately under its MIT license.
// Deliberate prototype limits are documented in docs/fable8-pocket.md.
#include "nucleo_pico8.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE 32768
#define SCREEN 0x6000
#define MAP 0x2000
#define FLAGS 0x3000
#define MAX_CODE 32768
#define HOOK_INTERVAL 1000
#define FRAME_INSTRUCTIONS 50000
#define LOAD_INSTRUCTIONS 200000

struct pico8 {
    // Keep this allocation below the device's ~32 KiB contiguous-block boundary.
    // The exact 32 KiB PICO RAM is a SECOND allocation (not one 33+ KiB struct).
    uint8_t *ram;
    lua_State *L;
    size_t used, peak, limit;
    uint32_t frames, rng;
    unsigned hz, hooks, native_work;
    int cam_x, cam_y, clip_x, clip_y, clip_w, clip_h, cursor_x, cursor_y;
    uint8_t color, held, pressed, ages[6], draw_pal[16], display_pal[16];
    uint16_t transparent;
    bool loaded, failed;
    const char *callback;
    char error[192];
};

static void *lua_alloc(void *ud, void *ptr, size_t old, size_t size) {
    pico8_t *p = ud;
    // Lua passes a type tag as osize for a new allocation, not a byte count.
    if (!ptr)
        old = 0;
    if (!size) {
        free(ptr);
        p->used -= old;
        return NULL;
    }
    if (size > p->limit || p->used - old > p->limit - size)
        return NULL;
    void *next = realloc(ptr, size);
    if (next) {
        p->used = p->used - old + size;
        if (p->used > p->peak)
            p->peak = p->used;
    }
    return next;
}

static pico8_t *machine(lua_State *L) { return *(pico8_t **)lua_getextraspace(L); }
static void instruction_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    pico8_t *p = machine(L);
    if (!p->hooks || !--p->hooks)
        luaL_error(L, "instruction budget exceeded");
}
static void set_error(pico8_t *p, const char *message) {
    snprintf(p->error, sizeof p->error, "%s", message ? message : "Lua error");
    p->failed = true;
}
static bool checked(pico8_t *p, int result) {
    if (result == LUA_OK)
        return true;
    set_error(p, lua_type(p->L, -1) == LUA_TSTRING ? lua_tostring(p->L, -1) : "non-string Lua error");
    lua_pop(p->L, 1);
    return false;
}

// Reject infinities/out-of-range conversions rather than invoking C float-to-int UB.
static int number(lua_State *L, int idx, int fallback) {
    if (lua_isnoneornil(L, idx))
        return fallback;
    lua_Number n = luaL_checknumber(L, idx);
    if (!isfinite(n) || n < -32768 || n > 32767)
        luaL_error(L, "numeric argument outside prototype range (-32768..32767)");
    return (int)floor(n);
}
static int clamp(int n, int lo, int hi) { return n < lo ? lo : n > hi ? hi : n; }
static void spend(pico8_t *p, unsigned work) {
    if (work > p->native_work)
        luaL_error(p->L, "native drawing budget exceeded");
    p->native_work -= work;
}
static uint8_t read_pixel(pico8_t *p, int base, int x, int y) {
    if (x < 0 || x >= 128 || y < 0 || y >= 128)
        return 0;
    uint8_t b = p->ram[base + y * 64 + x / 2];
    return (x & 1) ? b >> 4 : b & 15;
}
static void write_pixel(pico8_t *p, int base, int x, int y, int c) {
    if (x < 0 || x >= 128 || y < 0 || y >= 128)
        return;
    uint8_t *b = &p->ram[base + y * 64 + x / 2];
    *b = (x & 1) ? ((*b & 15) | ((c & 15) << 4)) : ((*b & 240) | (c & 15));
}
static void plot(pico8_t *p, int x, int y, int c) {
    spend(p, 1);
    x -= p->cam_x;
    y -= p->cam_y;
    if (x < p->clip_x || y < p->clip_y || x >= p->clip_x + p->clip_w || y >= p->clip_y + p->clip_h)
        return;
    write_pixel(p, SCREEN, x, y, p->draw_pal[c & 15]);
}
static int ink(lua_State *L, int idx) { return number(L, idx, machine(L)->color) & 15; }
static void line(pico8_t *p, int x0, int y0, int x1, int y1, int c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    // Argument limits bound this loop to 65536 pixels, even for off-screen lines.
    for (;;) {
        plot(p, x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e = 2 * err;
        if (e >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static int api_cls(lua_State *L) {
    pico8_t *p = machine(L);
    int c = p->draw_pal[number(L, 1, 0) & 15];
    spend(p, 8192);
    memset(p->ram + SCREEN, c | (c << 4), 8192);
    p->cursor_x = p->cursor_y = 0;
    p->clip_x = p->clip_y = 0;
    p->clip_w = p->clip_h = 128;
    return 0;
}
static int api_color(lua_State *L) {
    pico8_t *p = machine(L);
    int old = p->color;
    p->color = number(L, 1, 6) & 15;
    lua_pushinteger(L, old);
    return 1;
}
static int api_pset(lua_State *L) {
    plot(machine(L), number(L, 1, 0), number(L, 2, 0), ink(L, 3));
    return 0;
}
static int api_pget(lua_State *L) {
    pico8_t *p = machine(L);
    lua_pushinteger(L, read_pixel(p, SCREEN, number(L, 1, 0) - p->cam_x, number(L, 2, 0) - p->cam_y));
    return 1;
}
static int api_sget(lua_State *L) {
    lua_pushinteger(L, read_pixel(machine(L), 0, number(L, 1, 0), number(L, 2, 0)));
    return 1;
}
static int api_sset(lua_State *L) {
    write_pixel(machine(L), 0, number(L, 1, 0), number(L, 2, 0), ink(L, 3));
    return 0;
}
static int api_camera(lua_State *L) {
    pico8_t *p = machine(L);
    int x = number(L, 1, 0), y = number(L, 2, 0);
    lua_pushinteger(L, p->cam_x);
    lua_pushinteger(L, p->cam_y);
    p->cam_x = x;
    p->cam_y = y;
    return 2;
}
static int api_clip(lua_State *L) {
    pico8_t *p = machine(L);
    int x = number(L, 1, 0), y = number(L, 2, 0), w = number(L, 3, 128), h = number(L, 4, 128);
    p->clip_x = clamp(x, 0, 128);
    p->clip_y = clamp(y, 0, 128);
    p->clip_w = clamp(x + w, 0, 128) - p->clip_x;
    p->clip_h = clamp(y + h, 0, 128) - p->clip_y;
    return 0;
}
static int api_line(lua_State *L) {
    line(machine(L), number(L, 1, 0), number(L, 2, 0), number(L, 3, 0), number(L, 4, 0), ink(L, 5));
    return 0;
}
static int rectangle(lua_State *L, bool fill) {
    pico8_t *p = machine(L);
    int x0 = number(L, 1, 0), y0 = number(L, 2, 0), x1 = number(L, 3, 0), y1 = number(L, 4, 0), c = ink(L, 5);
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    if (!fill) {
        line(p, x0, y0, x1, y0, c);
        line(p, x0, y1, x1, y1, c);
        line(p, x0, y0, x0, y1, c);
        line(p, x1, y0, x1, y1, c);
    } else
        for (int y = clamp(y0 - p->cam_y, 0, 128); y <= clamp(y1 - p->cam_y, -1, 127); y++)
            for (int x = clamp(x0 - p->cam_x, 0, 128); x <= clamp(x1 - p->cam_x, -1, 127); x++)
                plot(p, x + p->cam_x, y + p->cam_y, c);
    return 0;
}
static int api_rect(lua_State *L) { return rectangle(L, false); }
static int api_rectfill(lua_State *L) { return rectangle(L, true); }
static int circle(lua_State *L, bool fill) {
    pico8_t *p = machine(L);
    int cx = number(L, 1, 0), cy = number(L, 2, 0), r = number(L, 3, 4), c = ink(L, 4);
    if (r < 0)
        return 0;
    // Rasterise only the visible rows/columns; huge radii cannot cause huge loops.
    double rr = (double)r * r;
    for (int y = 0; y < 128; y++) {
        int wy = y + p->cam_y;
        double dy = (double)wy - cy;
        if (dy * dy > rr)
            continue;
        int span = (int)floor(sqrt(rr - dy * dy));
        if (fill) {
            for (int x = clamp(cx - span - p->cam_x, 0, 128); x <= clamp(cx + span - p->cam_x, -1, 127); x++)
                plot(p, x + p->cam_x, wy, c);
        } else {
            plot(p, cx - span, wy, c);
            plot(p, cx + span, wy, c);
        }
    }
    if (!fill)
        for (int x = 0; x < 128; x++) {
            int wx = x + p->cam_x;
            double dx = (double)wx - cx;
            if (dx * dx <= rr) {
                int span = (int)floor(sqrt(rr - dx * dx));
                plot(p, wx, cy - span, c);
                plot(p, wx, cy + span, c);
            }
        }
    return 0;
}
static int api_circ(lua_State *L) { return circle(L, false); }
static int api_circfill(lua_State *L) { return circle(L, true); }
static int map_index(int x, int y) {
    if (x < 0 || x >= 128 || y < 0 || y >= 64)
        return -1;
    return y < 32 ? MAP + y * 128 + x : 0x1000 + (y - 32) * 128 + x;
}
static int api_mget(lua_State *L) {
    int i = map_index(number(L, 1, 0), number(L, 2, 0));
    lua_pushinteger(L, i < 0 ? 0 : machine(L)->ram[i]);
    return 1;
}
static int api_mset(lua_State *L) {
    int i = map_index(number(L, 1, 0), number(L, 2, 0));
    if (i >= 0)
        machine(L)->ram[i] = (uint8_t)number(L, 3, 0);
    return 0;
}
static int api_fget(lua_State *L) {
    int v = machine(L)->ram[FLAGS + (number(L, 1, 0) & 255)];
    if (lua_isnoneornil(L, 2))
        lua_pushinteger(L, v);
    else
        lua_pushboolean(L, (v & (1 << (number(L, 2, 0) & 7))) != 0);
    return 1;
}
static int api_fset(lua_State *L) {
    uint8_t *v = &machine(L)->ram[FLAGS + (number(L, 1, 0) & 255)];
    if (lua_gettop(L) < 3)
        *v = (uint8_t)number(L, 2, 0);
    else {
        int mask = 1 << (number(L, 2, 0) & 7);
        if (lua_toboolean(L, 3))
            *v |= mask;
        else
            *v &= ~mask;
    }
    return 0;
}
static void sprite(pico8_t *p, int sx, int sy, int sw, int sh, int x, int y, int dw, int dh, bool fx,
                   bool fy) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;
    // Clip destination first. A malicious cart cannot request a billion-pixel blit.
    for (int dy = clamp(p->cam_y - y, 0, dh); dy < clamp(128 + p->cam_y - y, 0, dh); dy++)
        for (int dx = clamp(p->cam_x - x, 0, dw); dx < clamp(128 + p->cam_x - x, 0, dw); dx++) {
            spend(p, 1); // transparent/clipped-source pixels still consume CPU
            int tx = (int)((int64_t)dx * sw / dw), ty = (int)((int64_t)dy * sh / dh);
            int px = sx + (fx ? sw - 1 - tx : tx), py = sy + (fy ? sh - 1 - ty : ty);
            if (px < 0 || px >= 128 || py < 0 || py >= 128)
                continue;
            int c = read_pixel(p, 0, px, py);
            if (!(p->transparent & (1 << c)))
                plot(p, x + dx, y + dy, c);
        }
}
static int api_spr(lua_State *L) {
    int n = number(L, 1, 0) & 255, w = clamp(number(L, 4, 1), 0, 16) * 8,
        h = clamp(number(L, 5, 1), 0, 16) * 8;
    sprite(machine(L), (n & 15) * 8, (n >> 4) * 8, w, h, number(L, 2, 0), number(L, 3, 0), w, h,
           lua_toboolean(L, 6), lua_toboolean(L, 7));
    return 0;
}
static int api_sspr(lua_State *L) {
    int sw = number(L, 3, 0), sh = number(L, 4, 0);
    sprite(machine(L), number(L, 1, 0), number(L, 2, 0), sw, sh, number(L, 5, 0), number(L, 6, 0),
           number(L, 7, sw), number(L, 8, sh), lua_toboolean(L, 9), lua_toboolean(L, 10));
    return 0;
}
static int api_map(lua_State *L) {
    pico8_t *p = machine(L);
    int mx = number(L, 1, 0), my = number(L, 2, 0), sx = number(L, 3, 0), sy = number(L, 4, 0);
    int w = clamp(number(L, 5, 16), 0, 128), h = clamp(number(L, 6, 16), 0, 64),
        layer = number(L, 7, 0) & 255;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (sx + x * 8 - p->cam_x >= 128 || sx + x * 8 + 8 - p->cam_x <= 0 ||
                sy + y * 8 - p->cam_y >= 128 || sy + y * 8 + 8 - p->cam_y <= 0)
                continue;
            int i = map_index(mx + x, my + y), n = i < 0 ? 0 : p->ram[i];
            if (n && (!layer || (p->ram[FLAGS + n] & layer) == layer))
                sprite(p, (n & 15) * 8, (n >> 4) * 8, 8, 8, sx + x * 8, sy + y * 8, 8, 8, false, false);
        }
    return 0;
}
static int api_pal(lua_State *L) {
    pico8_t *p = machine(L);
    if (!lua_gettop(L)) {
        for (int i = 0; i < 16; i++)
            p->draw_pal[i] = p->display_pal[i] = i;
        p->transparent = 1;
        return 0;
    }
    int a = number(L, 1, 0) & 15, b = number(L, 2, 0) & 15;
    if (number(L, 3, 0) == 1)
        p->display_pal[a] = b;
    else
        p->draw_pal[a] = b;
    return 0;
}
static int api_palt(lua_State *L) {
    pico8_t *p = machine(L);
    if (!lua_gettop(L)) {
        p->transparent = 1;
        return 0;
    }
    int bit = 1 << (number(L, 1, 0) & 15);
    if (lua_toboolean(L, 2))
        p->transparent |= bit;
    else
        p->transparent &= ~bit;
    return 0;
}

// An original tiny 3x5 uppercase font. Lowercase folds to uppercase in this prototype.
static const uint16_t font[36] = {0x7b6f, 0x2492, 0x73e7, 0x73cf, 0x5bc9, 0x79cf, 0x79ef, 0x7249, 0x7bef,
                                  0x7bcf, 0x2bed, 0x6bae, 0x7927, 0x6b6e, 0x79e7, 0x79e4, 0x796f, 0x5bed,
                                  0x7497, 0x126e, 0x5bad, 0x4927, 0x5fed, 0x5f6d, 0x7b6f, 0x7be4, 0x7b7b,
                                  0x7bad, 0x79cf, 0x7492, 0x5b6f, 0x5b6a, 0x5bfd, 0x5aad, 0x5a92, 0x72a7};
static int api_print(lua_State *L) {
    pico8_t *p = machine(L);
    int x = number(L, 2, p->cursor_x), y = number(L, 3, p->cursor_y), start = x, c = ink(L, 4);
    size_t len;
    const char *s = luaL_tolstring(L, 1, &len);
    // Bounded output; arbitrary long strings should not monopolise the app task.
    if (len > 1024)
        return luaL_error(L, "print string exceeds prototype limit (1024)");
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\n') {
            x = start;
            y += 6;
            continue;
        }
        if (ch >= 'a' && ch <= 'z')
            ch -= 32;
        int idx = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'Z' ? ch - 'A' + 10 : -1;
        uint16_t bits = idx < 0 ? 0 : font[idx];
        if (ch == '-')
            bits = 0x01c0;
        else if (ch == '.')
            bits = 2;
        else if (ch == ':')
            bits = 0x0410;
        else if (ch == '!')
            bits = 0x2482;
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (bits & (1 << (14 - row * 3 - col)))
                    plot(p, x + col, y + row, c);
        x += 4;
    }
    p->cursor_x = start;
    p->cursor_y = y + 6;
    lua_pushinteger(L, x);
    return 1;
}
static int api_cursor(lua_State *L) {
    pico8_t *p = machine(L);
    p->cursor_x = number(L, 1, 0);
    p->cursor_y = number(L, 2, 0);
    if (!lua_isnoneornil(L, 3))
        p->color = ink(L, 3);
    return 0;
}
static int button(lua_State *L, bool edge) {
    pico8_t *p = machine(L);
    uint8_t bits = edge ? p->pressed : p->held;
    if (number(L, 2, 0) != 0)
        bits = 0;
    if (lua_isnoneornil(L, 1))
        lua_pushinteger(L, bits);
    else {
        int b = number(L, 1, 0);
        lua_pushboolean(L, b >= 0 && b < 6 && (bits & (1 << b)));
    }
    return 1;
}
static int api_btn(lua_State *L) { return button(L, false); }
static int api_btnp(lua_State *L) { return button(L, true); }
static int api_time(lua_State *L) {
    pico8_t *p = machine(L);
    lua_pushnumber(L, (lua_Number)p->frames / p->hz);
    return 1;
}
static int api_flr(lua_State *L) {
    lua_pushnumber(L, floor(luaL_checknumber(L, 1)));
    return 1;
}
static int api_ceil(lua_State *L) {
    lua_pushnumber(L, ceil(luaL_checknumber(L, 1)));
    return 1;
}
static int api_abs(lua_State *L) {
    lua_pushnumber(L, fabs(luaL_checknumber(L, 1)));
    return 1;
}
static int api_min(lua_State *L) {
    lua_pushnumber(L, fmin(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
    return 1;
}
static int api_max(lua_State *L) {
    lua_pushnumber(L, fmax(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
    return 1;
}
static int api_mid(lua_State *L) {
    double a = luaL_checknumber(L, 1), b = luaL_checknumber(L, 2), c = luaL_checknumber(L, 3);
    lua_pushnumber(L, fmax(fmin(a, b), fmin(fmax(a, b), c)));
    return 1;
}
static int api_sqrt(lua_State *L) {
    lua_pushnumber(L, sqrt(fmax(0, luaL_checknumber(L, 1))));
    return 1;
}
static int api_sin(lua_State *L) {
    lua_pushnumber(L, -sin(luaL_checknumber(L, 1) * 6.283185307179586));
    return 1;
}
static int api_cos(lua_State *L) {
    lua_pushnumber(L, cos(luaL_checknumber(L, 1) * 6.283185307179586));
    return 1;
}
static uint32_t random32(pico8_t *p) {
    uint32_t x = p->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return p->rng = x;
}
static int api_srand(lua_State *L) {
    machine(L)->rng = (uint32_t)number(L, 1, 1);
    if (!machine(L)->rng)
        machine(L)->rng = 1;
    return 0;
}
static int api_rnd(lua_State *L) {
    double unit = (double)(random32(machine(L)) >> 8) / 16777216.0;
    if (lua_istable(L, 1)) {
        lua_Integer n = luaL_len(L, 1);
        if (!n)
            lua_pushnil(L);
        else
            lua_geti(L, 1, 1 + (lua_Integer)(unit * n));
    } else
        lua_pushnumber(L, unit * luaL_optnumber(L, 1, 1));
    return 1;
}
static int api_add(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Integer n = luaL_len(L, 1), at = luaL_optinteger(L, 3, n + 1);
    if (n < 0 || n > 4096 || at < 1 || at > n + 1)
        return luaL_error(L, "add table/position exceeds prototype limit");
    spend(machine(L), (unsigned)n + 1);
    for (lua_Integer i = n; i >= at; i--) {
        lua_geti(L, 1, i);
        lua_seti(L, 1, i + 1);
    }
    lua_pushvalue(L, 2);
    lua_seti(L, 1, at);
    lua_pushvalue(L, 2);
    return 1;
}
static int api_del(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Integer n = luaL_len(L, 1);
    if (n < 0 || n > 4096)
        return luaL_error(L, "del table exceeds prototype limit");
    spend(machine(L), (unsigned)n * 2 + 1);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_geti(L, 1, i);
        bool eq = lua_compare(L, -1, 2, LUA_OPEQ);
        lua_pop(L, 1);
        if (eq) {
            for (lua_Integer j = i; j < n; j++) {
                lua_geti(L, 1, j + 1);
                lua_seti(L, 1, j);
            }
            lua_pushnil(L);
            lua_seti(L, 1, n);
            lua_pushvalue(L, 2);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}
static int api_peek(lua_State *L) {
    int a = number(L, 1, 0);
    lua_pushinteger(L, a < 0 || a >= RAM_SIZE ? 0 : machine(L)->ram[a]);
    return 1;
}
static int api_poke(lua_State *L) {
    int a = number(L, 1, 0);
    for (int i = 2; i <= lua_gettop(L); i++, a++)
        if (a >= 0 && a < RAM_SIZE)
            machine(L)->ram[a] = (uint8_t)number(L, i, 0);
    return 0;
}
static int api_memcpy(lua_State *L) {
    int dst = number(L, 1, 0), src = number(L, 2, 0), n = number(L, 3, 0);
    if (dst < 0 || src < 0 || n < 0 || dst > RAM_SIZE - n || src > RAM_SIZE - n)
        return luaL_error(L, "memcpy outside PICO RAM");
    spend(machine(L), (unsigned)n);
    memmove(machine(L)->ram + dst, machine(L)->ram + src, (size_t)n);
    return 0;
}
static int api_memset(lua_State *L) {
    int dst = number(L, 1, 0), v = number(L, 2, 0), n = number(L, 3, 0);
    if (dst < 0 || n < 0 || dst > RAM_SIZE - n)
        return luaL_error(L, "memset outside PICO RAM");
    spend(machine(L), (unsigned)n);
    memset(machine(L)->ram + dst, v, (size_t)n);
    return 0;
}
static int api_unsupported(lua_State *L) {
    return luaL_error(L, "%s is not supported by this prototype", lua_tostring(L, lua_upvalueindex(1)));
}

static const luaL_Reg functions[] = {
    {"cls", api_cls},           {"color", api_color}, {"pset", api_pset},         {"pget", api_pget},
    {"sget", api_sget},         {"sset", api_sset},   {"camera", api_camera},     {"clip", api_clip},
    {"line", api_line},         {"rect", api_rect},   {"rectfill", api_rectfill}, {"circ", api_circ},
    {"circfill", api_circfill}, {"spr", api_spr},     {"sspr", api_sspr},         {"map", api_map},
    {"mget", api_mget},         {"mset", api_mset},   {"fget", api_fget},         {"fset", api_fset},
    {"pal", api_pal},           {"palt", api_palt},   {"print", api_print},       {"cursor", api_cursor},
    {"btn", api_btn},           {"btnp", api_btnp},   {"time", api_time},         {"t", api_time},
    {"flr", api_flr},           {"ceil", api_ceil},   {"abs", api_abs},           {"min", api_min},
    {"max", api_max},           {"mid", api_mid},     {"sqrt", api_sqrt},         {"sin", api_sin},
    {"cos", api_cos},           {"rnd", api_rnd},     {"srand", api_srand},       {"add", api_add},
    {"del", api_del},           {"peek", api_peek},   {"poke", api_poke},         {"memcpy", api_memcpy},
    {"memset", api_memset},     {NULL, NULL}};
static int bootstrap(lua_State *L) {
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1);
    lua_pop(L, 1);
    // No host filesystem, OS, packages, bytecode loading or script compilation entrypoints.
    // pcall/xpcall must not let carts swallow the instruction-limit exception.
    const char *remove[] = {"dofile", "loadfile", "load", "collectgarbage", "pcall", "xpcall", NULL};
    for (int i = 0; remove[i]; i++) {
        lua_pushnil(L);
        lua_setglobal(L, remove[i]);
    }
    for (const luaL_Reg *r = functions; r->name; r++) {
        lua_pushcfunction(L, r->func);
        lua_setglobal(L, r->name);
    }
    const char *unsupported[] = {"sfx",  "music", "cartdata", "dget", "dset",  "reload", "cstore",
                                 "load", "run",   "flip",     "stat", "tline", NULL};
    for (int i = 0; unsupported[i]; i++) {
        lua_pushstring(L, unsupported[i]);
        lua_pushcclosure(L, api_unsupported, 1);
        lua_setglobal(L, unsupported[i]);
    }
    return 0;
}
pico8_t *pico8_create(size_t limit) {
    if (limit < 8192)
        return NULL;
    // Claim the largest block FIRST, before even the small control structure can
    // carve a few bytes from the released 32 KiB UI canvas region.
    uint8_t *ram = calloc(1, RAM_SIZE);
    if (!ram)
        return NULL;
    pico8_t *p = calloc(1, sizeof *p);
    if (!p) {
        free(ram);
        return NULL;
    }
    p->ram = ram;
    p->limit = limit;
    p->hz = 30;
    p->color = 6;
    p->rng = 1;
    p->transparent = 1;
    p->clip_w = p->clip_h = 128;
    for (int i = 0; i < 16; i++)
        p->draw_pal[i] = p->display_pal[i] = i;
    p->L = lua_newstate(lua_alloc, p);
    if (!p->L) {
        pico8_destroy(p);
        return NULL;
    }
    *(pico8_t **)lua_getextraspace(p->L) = p;
    // Library/API setup can allocate too; protect it from Lua's panic/abort path.
    lua_pushcfunction(p->L, bootstrap);
    checked(p, lua_pcall(p->L, 0, 0, 0));
    lua_sethook(p->L, instruction_hook, LUA_MASKCOUNT, HOOK_INTERVAL);
    return p;
}
void pico8_destroy(pico8_t *p) {
    if (!p)
        return;
    if (p->L)
        lua_close(p->L);
    free(p->ram);
    free(p);
}
const char *pico8_error(const pico8_t *p) { return p ? p->error : "out of memory creating PICO-8 machine"; }
void pico8_stats(const pico8_t *p, pico8_stats_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    if (!p)
        return;
    out->lua_bytes = p->used;
    out->lua_peak = p->peak;
    out->lua_limit = p->limit;
    out->machine_bytes = sizeof *p + RAM_SIZE;
    out->frames = p->frames;
    out->update_hz = p->hz;
}

static int hex(int c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static bool section(const char *s) {
    // Only complete section marker lines delimit Lua. Long lines are chunked safely.
    size_t n = strcspn(s, "\r\n");
    if (n < 5 || n > 20 || s[0] != '_' || s[1] != '_' || s[n - 1] != '_' || s[n - 2] != '_')
        return false;
    for (size_t i = 2; i < n - 2; i++)
        if (s[i] < 'a' || s[i] > 'z')
            return false;
    return true;
}
typedef struct {
    FILE *f;
    char chunk[256];
    size_t bytes;
    bool line_start, done, too_large;
} code_reader_t;
static const char *read_code(lua_State *L, void *ud, size_t *size) {
    (void)L;
    code_reader_t *r = ud;
    *size = 0;
    if (r->done || !fgets(r->chunk, sizeof r->chunk, r->f))
        return NULL;
    if (r->line_start && section(r->chunk)) {
        r->done = true;
        return NULL;
    }
    size_t n = strlen(r->chunk);
    r->line_start = n && r->chunk[n - 1] == '\n';
    r->bytes += n;
    if (r->bytes > MAX_CODE) {
        r->too_large = true;
        return NULL;
    }
    *size = n;
    return r->chunk;
}
static int dispatch(lua_State *L) {
    pico8_t *p = machine(L);
    lua_getglobal(L, p->callback);
    if (lua_isnil(L, -1))
        return 0;
    if (!lua_isfunction(L, -1))
        return luaL_error(L, "cartridge callback is not a function");
    lua_call(L, 0, 0);
    return 0;
}
static int detect_rate(lua_State *L) {
    lua_getglobal(L, "_update60");
    if (lua_isfunction(L, -1))
        machine(L)->hz = 60;
    return 0;
}
static bool call(pico8_t *p, const char *name, unsigned budget) {
    p->callback = name;
    p->hooks = budget / HOOK_INTERVAL;
    p->native_work = 200000;
    lua_pushcfunction(p->L, dispatch);
    return checked(p, lua_pcall(p->L, 0, 0, 0));
}
bool pico8_load(pico8_t *p, const char *path) {
    if (!p || p->failed)
        return false;
    if (p->loaded) {
        set_error(p, "recreate machine before loading another cart");
        return false;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        set_error(p, "could not open .p8 cartridge");
        return false;
    }
    // Stream sections and stream Lua into its compiler: no full source/file allocation.
    char row[256];
    bool at_start = true, have_lua = false;
    long lua_offset = -1;
    int which = 0, index = 0, gfx_nibble = 0;
    if (!fgets(row, sizeof row, f) || strncmp(row, "pico-8 cartridge", 16)) {
        fclose(f);
        set_error(p, "expected a text .p8 cartridge (PNG is not supported)");
        return false;
    }
    while (fgets(row, sizeof row, f)) {
        bool marker = at_start && section(row);
        size_t n = strlen(row);
        at_start = n && row[n - 1] == '\n';
        if (marker) {
            which = 0;
            index = 0;
            gfx_nibble = 0;
            if (!strncmp(row, "__lua__", 7)) {
                if (have_lua) {
                    set_error(p, "duplicate Lua section");
                    break;
                }
                have_lua = true;
                lua_offset = ftell(f);
            } else if (!strncmp(row, "__gfx__", 7))
                which = 1;
            else if (!strncmp(row, "__map__", 7))
                which = 2;
            else if (!strncmp(row, "__gff__", 7))
                which = 3;
            // SFX/music data may be present, but APIs explicitly fail until implemented.
            continue;
        }
        if (!which)
            continue;
        for (size_t j = 0; j < n; j++) {
            int digit = hex((unsigned char)row[j]);
            if (digit < 0) {
                if (row[j] == '\r' || row[j] == '\n' || row[j] == ' ')
                    continue;
                set_error(p, "invalid hex in cartridge data");
                break;
            }
            int max = which == 1 ? 16384 : which == 2 ? 8192 : 512;
            if (index >= max) {
                set_error(p, "cartridge data section too large");
                break;
            }
            int base = which == 1 ? 0 : which == 2 ? MAP : FLAGS;
            if (which == 1) { // .p8 graphics spell pixels low nibble first.
                if (!(index & 1))
                    p->ram[index / 2] = (uint8_t)digit;
                else
                    p->ram[index / 2] |= (uint8_t)(digit << 4);
            } else {
                if (!(index & 1))
                    gfx_nibble = digit;
                else
                    p->ram[base + index / 2] = (uint8_t)((gfx_nibble << 4) | digit);
            }
            index++;
        }
        if (p->failed)
            break;
    }
    if (ferror(f))
        set_error(p, "cartridge read failed");
    if (!p->failed && (!have_lua || lua_offset < 0))
        set_error(p, "cartridge has no Lua section");
    if (p->failed) {
        fclose(f);
        return false;
    }
    if (fseek(f, lua_offset, SEEK_SET)) {
        fclose(f);
        set_error(p, "could not seek cartridge Lua");
        return false;
    }
    code_reader_t reader = {.f = f, .line_start = true};
    int result = lua_load(p->L, read_code, &reader, path, "t");
    fclose(f);
    if (!checked(p, result))
        return false;
    if (reader.too_large) {
        lua_pop(p->L, 1);
        set_error(p, "Lua source exceeds prototype limit (32 KiB)");
        return false;
    }
    p->hooks = LOAD_INSTRUCTIONS / HOOK_INTERVAL;
    p->native_work = 200000;
    if (!checked(p, lua_pcall(p->L, 0, 0, 0)))
        return false;
    if (!call(p, "_init", LOAD_INSTRUCTIONS))
        return false;
    lua_pushcfunction(p->L, detect_rate);
    if (!checked(p, lua_pcall(p->L, 0, 0, 0)))
        return false;
    p->loaded = true;
    return true;
}
bool pico8_frame(pico8_t *p, uint8_t held) {
    if (!p || !p->loaded || p->failed)
        return false;
    p->held = held & 63;
    p->pressed = 0;
    for (int b = 0; b < 6; b++) {
        if (!(p->held & (1 << b))) {
            p->ages[b] = 0;
            continue;
        }
        unsigned age = p->ages[b];
        unsigned delay = p->hz == 60 ? 30 : 15, interval = p->hz == 60 ? 8 : 4;
        if (!age || (age >= delay && (age - delay) % interval == 0))
            p->pressed |= 1 << b;
        // Saturate by wrapping back to the repeat phase, not by stopping repeats.
        p->ages[b] = (uint8_t)(age >= delay + interval ? delay + 1 : age + 1);
    }
    if (!call(p, p->hz == 60 ? "_update60" : "_update", FRAME_INSTRUCTIONS) ||
        !call(p, "_draw", FRAME_INSTRUCTIONS))
        return false;
    p->frames++;
    return true;
}
uint8_t pico8_pixel(const pico8_t *p, unsigned x, unsigned y) {
    if (!p || x >= 128 || y >= 128)
        return 0;
    uint8_t b = p->ram[SCREEN + y * 64 + x / 2];
    return x & 1 ? b >> 4 : b & 15;
}
void pico8_scanline(const pico8_t *p, unsigned y, uint16_t out[128]) {
    static const uint16_t palette[16] = {0x0000, 0x194a, 0x792a, 0x042a, 0xaa86, 0x5aa9, 0xc618, 0xff9d,
                                         0xf809, 0xfd00, 0xff64, 0x0726, 0x2d7f, 0x83b3, 0xfbb5, 0xfe75};
    for (unsigned x = 0; x < 128; x++)
        out[x] = palette[p ? p->display_pal[pico8_pixel(p, x, y)] : 0];
}
