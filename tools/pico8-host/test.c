#include "nucleo_pico8.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int count;
static char path[512];
static void pass(const char *name) { printf("PASS %02d %s\n", ++count, name); }
static pico8_t *fresh(size_t limit) {
    pico8_t *p = pico8_create(limit);
    assert(p);
    if (pico8_error(p)[0])
        fprintf(stderr, "create: %s\n", pico8_error(p));
    assert(!pico8_error(p)[0]);
    return p;
}
static void cart(const char *code, const char *sections) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("pico-8 cartridge // test\nversion 42\n__lua__\n", f);
    fputs(code, f);
    fputs("\n", f);
    if (sections)
        fputs(sections, f);
    assert(!fclose(f));
}
static void works(const char *name, const char *code) {
    cart(code, NULL);
    pico8_t *p = fresh(32768);
    bool ok = pico8_load(p, path);
    if (!ok)
        fprintf(stderr, "%s: %s\n", name, pico8_error(p));
    assert(ok);
    assert(pico8_frame(p, 0));
    pico8_destroy(p);
    pass(name);
}
static void refuses(const char *name, const char *code, const char *expect, bool frame) {
    cart(code, NULL);
    pico8_t *p = fresh(32768);
    bool ok = pico8_load(p, path);
    if (frame) {
        assert(ok);
        ok = pico8_frame(p, 0);
    }
    assert(!ok);
    if (!strstr(pico8_error(p), expect))
        fprintf(stderr, "%s: expected %s, got %s\n", name, expect, pico8_error(p));
    assert(strstr(pico8_error(p), expect));
    pico8_stats_t stats;
    pico8_stats(p, &stats);
    assert(stats.lua_peak <= stats.lua_limit);
    pico8_destroy(p);
    pass(name);
}
int main(int argc, char **argv) {
    assert(argc == 4);
    snprintf(path, sizeof path, "%s/test.p8", argv[1]);
    works("basic drawing and default print arguments",
          "function _draw() cls(1) pset(3,4,8) assert(pget(3,4)==8) rectfill(10,10,12,12,9) "
          "assert(pget(11,11)==9) print('hello') end");
    works("camera reset and clipping",
          "cls() camera(2,3) pset(5,7,9) camera() assert(pget(3,4)==9) clip(10,10,2,2) pset(0,0,8) "
          "assert(pget(0,0)==0) pset(10,10,7) assert(pget(10,10)==7)");
    works("sprite nibble order, scaling, transparency and palettes",
          "cls(1) sset(0,0,8) sset(1,0,0) spr(0,20,20) assert(pget(20,20)==8) assert(pget(21,20)==1) "
          "pal(8,9) spr(0,30,20) assert(pget(30,20)==9) sspr(0,0,1,1,40,40,2,2) assert(pget(41,41)==9)");
    works("map shared space, flags and tile rendering",
          "cls() sset(8,0,7) mset(0,0,1) fset(1,2,true) assert(fget(1,2)) assert(fget(1)==4) "
          "map(0,0,10,10,1,1,4) assert(pget(10,10)==7) mset(0,32,42) assert(peek(4096)==42)");
    works("circle, line and reversed rectangle",
          "cls() circfill(20,20,3,8) assert(pget(20,20)==8) line(1,1,4,4,7) assert(pget(2,2)==7) "
          "rect(8,8,6,6,9) assert(pget(6,6)==9)");
    works("bounded memory functions and overlap", "poke(100,1,2,3) memcpy(101,100,3) assert(peek(103)==3) "
                                                  "memset(100,9,4) assert(peek(102)==9) assert(peek(-1)==0)");
    works("math helpers, deterministic random and table operations",
          "assert(flr(-1.1)==-2 and ceil(1.1)==2) assert(mid(9,2,5)==5) assert(abs(sin(0.25)+1)<0.001) "
          "srand(9) a=rnd() srand(9) assert(a==rnd()) a={} add(a,4) add(a,3,1) assert(a[1]==3) "
          "assert(del(a,4)==4 and #a==1)");
    works("no host filesystem, OS, modules or catchable budget errors",
          "assert(io==nil and os==nil and package==nil and debug==nil and dofile==nil and loadfile==nil and "
          "pcall==nil and xpcall==nil)");

    cart("function _update() if btn(0) then pset(2,2,8) end if btnp(0) then poke(100,peek(100)+1) end end "
         "function _draw() if time()>0.62 then assert(peek(100)==3) end end",
         NULL);
    pico8_t *p = fresh(32768);
    assert(pico8_load(p, path));
    for (int i = 0; i < 20; i++)
        assert(pico8_frame(p, 1));
    assert(pico8_pixel(p, 2, 2) == 8);
    pico8_stats_t stats;
    pico8_stats(p, &stats);
    assert(stats.frames == 20 && stats.update_hz == 30);
    pico8_destroy(p);
    pass("held input, repeated frames and statistics");
    works("60 Hz callback selection",
          "function _update60() pset(1,1,8) end function _draw() assert(pget(1,1)==8) end");
    cart("function _update() assert(btnp(0)==(time()==0)) end", NULL);
    p = fresh(32768);
    assert(pico8_load(p, path));
    assert(pico8_frame(p, 1));
    assert(pico8_frame(p, 1));
    pico8_destroy(p);
    pass("btnp first press is an edge");

    cart("assert(sget(0,0)==1 and sget(1,0)==2) assert(mget(0,0)==0x12) assert(fget(0)==0x34)",
         "__gfx__\n12\n__map__\n12\n__gff__\n34\n");
    p = fresh(32768);
    assert(pico8_load(p, path));
    pico8_destroy(p);
    pass("streamed .p8 section decoding");
    refuses("syntax errors are reported", "function _draw( end", "expected", false);
    refuses("unsupported audio is explicit", "sfx(0)", "not supported", false);
    refuses("unsupported saves are explicit", "cartdata('x')", "not supported", false);
    refuses("instruction budget stops top-level loops", "while true do end", "instruction budget", false);
    refuses("instruction budget stops frame loops", "function _update() while true do end end",
            "instruction budget", true);
    refuses("Lua allocations are capped", "x=string.rep('x',1000000)", "memory", false);
    refuses("out-of-range host memory is rejected", "memcpy(-1,0,100)", "outside PICO RAM", false);
    refuses("invalid numeric conversion is rejected", "pset(0/0,1,8)", "numeric argument", false);
    refuses("non-string Lua errors are handled", "error({})", "non-string Lua error", false);
    refuses("bad callback types are handled", "_update=4", "not a function", true);
    refuses("native raster work is bounded", "for i=1,30 do cls(1) end", "native drawing budget", false);
    refuses("transparent sprites also consume the native work budget",
            "for i=1,20 do sspr(0,0,128,128,0,0) end", "native drawing budget", false);
    works("streaming preserves long Lua lines",
          "local text='abcdefghijklmnopqrstuvwxyz'..string.rep('x',2000) assert(#text==2026)");
    char nested[512];
    strcpy(nested, "local a=");
    for (int i = 0; i < 100; i++)
        strcat(nested, "(");
    strcat(nested, "1");
    for (int i = 0; i < 100; i++)
        strcat(nested, ")");
    refuses("parser recursion is bounded", nested, "C stack overflow", false);
    cart("a=1", "__lua__\nb=2\n");
    p = fresh(32768);
    assert(!pico8_load(p, path));
    assert(strstr(pico8_error(p), "duplicate"));
    pico8_destroy(p);
    pass("duplicate Lua sections rejected");
    cart("a=1", "__gfx__\nxyz\n");
    p = fresh(32768);
    assert(!pico8_load(p, path));
    assert(strstr(pico8_error(p), "invalid hex"));
    pico8_destroy(p);
    pass("malformed graphics rejected");
    cart("a=1", "__gfx__\n");
    FILE *large = fopen(path, "a");
    assert(large);
    for (int i = 0; i < 16386; i++)
        fputc('1', large);
    fclose(large);
    p = fresh(32768);
    assert(!pico8_load(p, path));
    assert(strstr(pico8_error(p), "too large"));
    pico8_destroy(p);
    pass("oversized graphics rejected before writing beyond sprite RAM");
    cart("-- source-size check", NULL);
    large = fopen(path, "a");
    assert(large);
    for (int i = 0; i < 4000; i++)
        fputs("-- comment\n", large);
    fclose(large);
    p = fresh(32768);
    assert(!pico8_load(p, path));
    assert(strstr(pico8_error(p), "32 KiB"));
    pico8_destroy(p);
    pass("oversized streamed Lua source rejected");
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("not a cartridge", f);
    fclose(f);
    p = fresh(32768);
    assert(!pico8_load(p, path));
    pico8_destroy(p);
    pass("non-text cartridges rejected");
    p = fresh(32768);
    assert(!pico8_load(p, "/nonexistent/cart.p8"));
    pico8_destroy(p);
    pass("missing cartridge error");
    // Repeat create/destroy under ASan/LSan to exercise cleanup on both success and failures.
    for (int i = 0; i < 50; i++) {
        p = fresh(32768);
        pico8_destroy(p);
    }
    pass("repeated machine lifecycle");
    assert(!pico8_create(1024));
    pass("impossible Lua budget rejected");
    for (size_t limit = 8192; limit < 32768; limit += 1024) {
        p = pico8_create(limit);
        if (p) {
            pico8_stats(p, &stats);
            assert(stats.lua_peak <= limit);
            pico8_destroy(p);
        }
    }
    pass("library setup handles small allocator budgets without a Lua panic");

    p = fresh(32768);
    assert(pico8_load(p, argv[2]));
    for (int i = 0; i < 120; i++)
        assert(pico8_frame(p, i < 10 ? 2 : 0));
    assert(pico8_pixel(p, 80, 65) == 12);
    assert(pico8_pixel(p, 60, 65) != 12);
    pico8_stats(p, &stats);
    assert(stats.lua_peak <= 32768);
    printf("DEMO machine=%zu lua_current=%zu lua_peak=%zu lua_limit=%zu frames=%u hz=%u\n",
           stats.machine_bytes, stats.lua_bytes, stats.lua_peak, stats.lua_limit, stats.frames,
           stats.update_hz);
    f = fopen(argv[3], "wb");
    assert(f);
    fputs("P6\n128 128\n255\n", f);
    for (unsigned y = 0; y < 128; y++) {
        uint16_t row[128];
        pico8_scanline(p, y, row);
        for (unsigned x = 0; x < 128; x++) {
            unsigned v = row[x];
            unsigned char rgb[3] = {(unsigned char)(((v >> 11) & 31) * 255 / 31),
                                    (unsigned char)(((v >> 5) & 63) * 255 / 63),
                                    (unsigned char)((v & 31) * 255 / 31)};
            assert(fwrite(rgb, 1, 3, f) == 3);
        }
    }
    assert(!fclose(f));
    pico8_destroy(p);
    pass("playable demo: 120 frames, input moves player, rendered PPM");
    printf("%d tests passed\n", count);
    return 0;
}
