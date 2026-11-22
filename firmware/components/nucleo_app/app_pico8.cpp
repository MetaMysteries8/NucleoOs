// FABLE-8 Pocket: on-device PICO-8 subset. No browser, cloud or background state.
#include "sdkconfig.h"
#if CONFIG_NUCLEO_PICO8_PROTOTYPE
#include "app_gfx.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nucleo_app.h"
#include "nucleo_exclusive.h"
#include "nucleo_i18n.h"
#include "nucleo_pico8.h"
#include "nucleo_theme.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

namespace {
constexpr const char *ROOT = "/sd/Games/PICO8";
constexpr uint16_t ERROR_COLOR = 0xf986;
constexpr size_t LUA_LIMIT = 32 * 1024;
constexpr int MAX_CARTS = 20;
struct Session {
    pico8_t *machine;
    pico8_t *prepared;
    char names[MAX_CARTS][64];
    char path[192], error[192];
    int count, selected;
    bool paused, repaint, truncated;
    int64_t next_frame, next_report;
    uint32_t last_us;
    uint16_t line[128];
};
// Only a pointer remains while closed; all actual state is heap-on-enter.
static Session *session;
extern const char demo_start[] asm("_binary_pocket_demo_p8_start");
extern const char demo_end[] asm("_binary_pocket_demo_p8_end");

static void memory_report(const char *stage) {
    pico8_stats_t stats{};
    pico8_stats(session ? (session->machine ? session->machine : session->prepared) : nullptr, &stats);
    // printf is intentional: the normal firmware compiles ESP_LOGI out at WARN.
    printf("[pico8] %s free=%u largest=%u machine=%u lua=%u peak=%u limit=%u frames=%lu frame_us=%lu "
           "stack_free_min=%u\n",
           stage, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)stats.machine_bytes,
           (unsigned)stats.lua_bytes, (unsigned)stats.lua_peak, (unsigned)stats.lua_limit,
           (unsigned long)stats.frames, (unsigned long)(session ? session->last_us : 0),
           (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}
static void install_demo() {
    mkdir("/sd/Games", 0775);
    mkdir(ROOT, 0775);
    const char *path = "/sd/Games/PICO8/pocket-demo.p8";
    struct stat sb;
    if (!stat(path, &sb))
        return; // never overwrite the owner's cart
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    // EMBED_TXTFILES includes a trailing NUL; cartridge files do not.
    size_t length = (size_t)(demo_end - demo_start);
    if (length && demo_start[length - 1] == '\0')
        --length;
    bool ok = fwrite(demo_start, 1, length, f) == length;
    if (fclose(f))
        ok = false;
    if (!ok)
        remove(path);
}
static void scan() {
    session->count = 0;
    session->truncated = false;
    DIR *dir = opendir(ROOT);
    if (!dir) {
        snprintf(session->error, sizeof session->error, "Insert SD card. Carts: %s", ROOT);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        size_t n = strlen(entry->d_name);
        if (n < 4 || strcasecmp(entry->d_name + n - 3, ".p8"))
            continue;
        if (n >= sizeof session->names[0] || session->count == MAX_CARTS) {
            session->truncated = true;
            continue;
        }
        strcpy(session->names[session->count++], entry->d_name);
    }
    closedir(dir);
    for (int i = 0; i < session->count; ++i)
        for (int j = i + 1; j < session->count; ++j)
            if (strcasecmp(session->names[i], session->names[j]) > 0) {
                char tmp[64];
                strcpy(tmp, session->names[i]);
                strcpy(session->names[i], session->names[j]);
                strcpy(session->names[j], tmp);
            }
    if (session->selected >= session->count)
        session->selected = 0;
    session->repaint = true;
}
static void stop(bool reserve = true) {
    if (!session)
        return;
    pico8_destroy(session->machine);
    session->machine = nullptr;
    if (reserve && !session->prepared)
        session->prepared = pico8_create(LUA_LIMIT);
    session->paused = false;
    session->repaint = true;
    memory_report("stop");
}
static void start() {
    if (!session || !session->count)
        return;
    stop();
    session->error[0] = '\0';
    snprintf(session->path, sizeof session->path, "%s/%s", ROOT, session->names[session->selected]);
    memory_report("before-load");
    session->machine = session->prepared;
    session->prepared = nullptr;
    if (!session->machine)
        session->machine = pico8_create(LUA_LIMIT);
    if (!session->machine || !pico8_load(session->machine, session->path)) {
        snprintf(session->error, sizeof session->error, "%s", pico8_error(session->machine));
        memory_report("load-error");
        stop();
    }
    session->next_frame = esp_timer_get_time();
    session->next_report = session->next_frame + 5000000;
    memory_report("loaded");
    nucleo_app_request_draw();
}
static uint8_t held() {
    uint8_t bits = 0;
    if (nucleo_kbd_char_down('a') || nucleo_kbd_char_down(','))
        bits |= 1;
    if (nucleo_kbd_char_down('d') || nucleo_kbd_char_down('/'))
        bits |= 2;
    if (nucleo_kbd_char_down('e') || nucleo_kbd_char_down(';'))
        bits |= 4;
    if (nucleo_kbd_char_down('s') || nucleo_kbd_char_down('.'))
        bits |= 8;
    if (nucleo_kbd_char_down('j') || nucleo_kbd_char_down('z'))
        bits |= 16;
    if (nucleo_kbd_char_down('k') || nucleo_kbd_char_down('x'))
        bits |= 32;
    return bits;
}
static bool poll() {
    if (!session || !session->machine || session->paused)
        return false;
    int64_t now = esp_timer_get_time();
    if (now < session->next_frame)
        return false;
    if (!pico8_frame(session->machine, held())) {
        snprintf(session->error, sizeof session->error, "%s", pico8_error(session->machine));
        memory_report("frame-error");
        stop();
        return true;
    }
    session->last_us = (uint32_t)(esp_timer_get_time() - now);
    pico8_stats_t stats{};
    pico8_stats(session->machine, &stats);
    int64_t interval = 1000000 / stats.update_hz;
    session->next_frame += interval;
    // One update per poll; do not pile up catch-up frames and starve the OS.
    if (session->next_frame < now)
        session->next_frame = now + interval;
    if (now >= session->next_report) {
        memory_report("play");
        session->next_report = now + 5000000;
    }
    return true;
}
static bool back(int key) {
    if (!session)
        return false;
    // The framework routes LEFT here too; it remains a game direction.
    if (key == NK_LEFT)
        return session->machine != nullptr;
    if (session->machine) {
        stop();
        nucleo_app_request_draw();
        return true;
    }
    if (session->error[0]) {
        session->error[0] = '\0';
        session->repaint = true;
        nucleo_app_request_draw();
        return true;
    }
    return false;
}
static void key(int k, char ch) {
    if (!session)
        return;
    if (session->machine) {
        if (ch == 'p' || ch == 'P') {
            session->paused = !session->paused;
            session->next_frame = esp_timer_get_time();
            session->repaint = true;
        }
        if (ch == 'r' || ch == 'R')
            start();
    } else {
        if (k == NK_UP && session->selected > 0)
            --session->selected;
        if (k == NK_DOWN && session->selected + 1 < session->count)
            ++session->selected;
        if (k == NK_ENTER)
            start();
        if (ch == 'r' || ch == 'R') {
            session->error[0] = '\0';
            scan();
        }
        session->repaint = true;
    }
    nucleo_app_request_draw();
}
static void draw() {
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextDatum(top_left);
    if (!session) {
        d.fillScreen(0);
        d.setTextColor(THEME_FG, THEME_BG);
        d.drawString("FABLE-8: no session RAM", 4, 30);
        return;
    }
    if (session->repaint) {
        d.fillScreen(0);
        session->repaint = false;
    }
    if (session->machine) {
        int x = (d.width() - 128) / 2, y = (d.height() - 128) / 2;
        d.startWrite();
        for (unsigned row = 0; row < 128; row++) {
            pico8_scanline(session->machine, row, session->line);
            d.pushImage(x, y + (int)row, 128, 1, (const lgfx::rgb565_t *)session->line);
        }
        d.endWrite();
        if (session->paused) {
            d.setTextColor(THEME_FG, THEME_BG);
            d.drawString(TR("PAUSA", "PAUSED"), x + 39, y + 60);
        }
        return;
    }
    d.fillScreen(THEME_BG);
    d.setTextColor(THEME_FG, THEME_BG);
    d.drawString("FABLE-8 POCKET / prototype", 4, 3);
    d.setTextColor(THEME_MUTED, THEME_BG);
    d.drawString(TR("P8 ridotto | no audio/salvataggi", "P8 subset | no sound or saves yet"), 4, 16);
    if (session->error[0]) {
        d.setTextColor(ERROR_COLOR, THEME_BG);
        // Bound each line to the original Cardputer's width. No long error disappears off-screen.
        for (int i = 0; i < 5; i++) {
            char row[37];
            size_t offset = (size_t)i * 36;
            if (offset >= strlen(session->error))
                break;
            snprintf(row, sizeof row, "%.36s", session->error + offset);
            d.drawString(row, 4, 33 + i * 13);
        }
        d.setTextColor(THEME_MUTED, THEME_BG);
        d.drawString(TR("Esc chiudi | R rileggi", "Esc dismiss | R rescan"), 4, 119);
        return;
    }
    int first = (session->selected / 6) * 6;
    for (int i = first; i < session->count && i < first + 6; i++) {
        int y = 32 + (i - first) * 13;
        d.setTextColor(i == session->selected ? THEME_ACC : THEME_FG, THEME_BG);
        char row[38];
        snprintf(row, sizeof row, "%c %.34s", i == session->selected ? '>' : ' ', session->names[i]);
        d.drawString(row, 4, y);
    }
    if (!session->count)
        d.drawString(TR("Nessun .p8 in /Games/PICO8", "No .p8 carts in /Games/PICO8"), 4, 40);
    d.setTextColor(THEME_MUTED, THEME_BG);
    d.drawString(session->truncated
                     ? TR("Prime 20 cartucce / nomi brevi", "First 20 carts / short names only")
                     : TR("Invio gioca | R rileggi | Esc esci", "Enter play | R rescan | Esc quit"),
                 4, 119);
}
static void enter() {
    nucleo_app_set_fullscreen(true);
    nucleo_screen_hold(true);
    nucleo_app_release_buffers();
    nucleo_screen_release();
    // Reserve PICO RAM before allocating the cart list: keeping an empty VM on
    // the shelf stops menu/font allocations fragmenting the only 32 KiB block.
    pico8_t *prepared = pico8_create(LUA_LIMIT);
    session = (Session *)calloc(1, sizeof(Session));
    if (!session) {
        pico8_destroy(prepared);
        return;
    }
    session->prepared = prepared;
    memory_report("enter");
    install_demo();
    scan();
    nucleo_app_set_back_handler(back);
    nucleo_app_set_poll_handler(poll);
}
static void leave() {
    stop(false);
    if (session)
        pico8_destroy(session->prepared);
    free(session);
    session = nullptr;
    nucleo_app_set_poll_handler(nullptr);
    nucleo_app_set_back_handler(nullptr);
    nucleo_screen_hold(false);
    nucleo_app_set_fullscreen(false);
}
} // namespace
extern "C" void nucleo_register_pico8(void) {
    static const nucleo_app_def_t app = {
        "pico8", "FABLE-8 Pocket",
        "Games", "Experimental on-device PICO-8 subset. Text .p8 carts; no sound or saves yet.",
        '8',     0x2d7f,
        enter,   key,
        nullptr, draw,
        leave,   NX_SOLO | NX_DEEP_OFFLINE,
        nullptr, nullptr};
    nucleo_app_register(&app);
}
#endif // CONFIG_NUCLEO_PICO8_PROTOTYPE
