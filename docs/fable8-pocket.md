# FABLE-8 Pocket — first native prototype

An experimental **on-device PICO-8 subset** for the original Cardputer. No
browser, host computer, Internet connection, telemetry or account is involved
in running a cartridge. This is not the official PICO-8 runtime and does not
claim general cartridge compatibility.

## What works

- Games → **FABLE-8 Pocket** in the native launcher.
- Text `.p8` carts from `/Games/PICO8/` on SD; a small paged picker (first 20
  matching filenames returned by the directory, sorted; names under 64 bytes).
- On first open, installs the original `pocket-demo.p8` into that folder if
  absent. An existing file is never overwritten.
- A real capped Lua 5.4.8 interpreter; `_init`, `_update`, `_update60`, `_draw`.
- A packed 128×128 framebuffer displayed 1:1, centered on the 240×135 panel.
  Only a 256-byte RGB565 scanline is converted for transfer.
- Basic drawing, camera, clipping, sprites, scaled sprites, maps, flags,
  palette/transparency, `btn`/`btnp`, common math/table helpers, and bounded
  reads/writes inside the 32 KiB emulated address space.
- Pause, restart, cart errors and return to the picker/OS.

The bundled game is a functioning input/rendering test: move the blue player
into yellow stars; O changes the player's color, X resets the score. It uses
standard Lua syntax deliberately.

## Controls

| Action | Keys |
| --- | --- |
| Select cartridge | Up/Down; Enter plays |
| Left / Right | A / D, or printed `,` / `/` arrow legends |
| Up / Down | E / S, or printed `;` / `.` arrow legends |
| O / X | J or Z / K or X |
| Pause / resume | P |
| Restart cartridge | R |
| Return to picker | Esc |
| Rescan picker | R |
| Quit from picker | Esc; after an error, first Esc dismisses it |

Held buttons are read from the physical key set, so simultaneous movement and
action buttons work. The main loop polls the emulator; requested 30/60 Hz
callback timing is subject to actual loop/display/CPU speed. The `_update60`
API is implemented; 60 physical frames per second are **not** promised.

## Explicit limitations

- **No sound or saves yet.** `sfx`, `music`, `cartdata`, `dget`, `dset`, `reload`,
  `cstore`, cart chaining, `flip`, `stat` and `tline` report an unsupported-API
  error rather than silently pretending to work.
- Lua 5.4 with 32-bit integers/floats, **not** PICO-8 fixed-point 16.16 math.
  No shorthand `+=`, `!=`, single-line `if`, `?`, P8SCII text, or dialect-specific
  bitwise/number semantics. Ordinary Lua syntax using the supported APIs works.
- The font is an original tiny uppercase 3×5 font, not a PICO-8 font reproduction.
- No `.p8.png`, editor suite, online browser, multiplayer or full save states.
- Peek/poke cover only the allocated 0x0000–0x7fff RAM. Draw state is maintained
  separately; poking hardware/draw-state registers does not implement those
  register semantics. Upper-map/sprite sharing is implemented.
- Lua standard libraries are a small convenience subset, not API-equivalent
  replacements for all PICO-8 functions. Per-call argument bounds can reject
  carts that the full runtime accepts.
- A single cart can still exceed the available physical heap. OOM is an error,
  not evidence that the program fits because its `.p8` file is small.

## Memory and execution budget

The machine reserves the exact 32 KiB RAM block **before** allocating smaller
control structures. The app releases/holds off NucleoOs's shared UI canvas and
uses `NX_SOLO | NX_DEEP_OFFLINE`. An empty VM is retained on the picker so that
menu allocations cannot carve up the one contiguous block before a cart starts.
All machine, Lua and app allocations are released on exit. No large mutable
arrays exist while the app is closed.

Lua allocations are capped at **32 KiB**, including compilation; its compiler
reads the `.p8` source in small chunks instead of retaining the complete file.
Lua source is limited to 32 KiB. Data sections are bounds-checked. Lua callbacks
have a 50,000-instruction budget each (200,000 for top-level/init); native
raster work is bounded separately to 200,000 operations per callback. Parser/
C-call recursion is capped at 40. `pcall`/`xpcall` are not exposed, so carts
cannot intentionally swallow the instruction-budget exception.

No Lua I/O, OS, package, debug or coroutine library is exposed. These limits
are resource guards, not a claim of a security sandbox for hostile cartridges.

Serial `[pico8]` reports at enter/load/stop and roughly every five seconds
include free internal heap, largest block, current/peak Lua bytes, machine
bytes, frame count, task stack minimum free bytes and Lua update/draw time.
`frame_us` excludes display transfer. A hardware session must measure actual
displayed frame rate too before making performance claims.

## Build and test

Work from this feature branch of https://github.com/MetaMysteries8/NucleoOs.
SDK pin at the starting revision: **ESP-IDF v5.4.4**, `esp32s3`. Recheck CI and
lockfiles when rebasing. Do not substitute the latest IDF release.

Official SDK instructions:
https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/get-started/linux-macos-setup.html

After installing the documented host prerequisites:

```sh
mkdir -p "$HOME/esp"
git clone --branch v5.4.4 --recursive https://github.com/espressif/esp-idf.git "$HOME/esp/esp-idf-v5.4.4"
cd "$HOME/esp/esp-idf-v5.4.4"
./install.sh esp32s3
. ./export.sh
```

The experiment is **off by default**. For a separate build/configuration,
run from the NucleoOs checkout:

```sh
cd firmware
idf.py -B build-pico8 -D IDF_TARGET=esp32s3 -D SDKCONFIG=sdkconfig.pico8 -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.pico8.defaults" build
cd ..
node tools/image-size-check.mjs firmware/build-pico8/nucleoos.bin
npm run pico8:test
npm run launcher:test
```

For a fresh configuration set the target to `esp32s3` using the normal SDK
instructions. Preserve existing configuration first: `set-target` resets it.
You can also enable "Experimental FABLE-8 Pocket player" under the native-app
menu in `idf.py menuconfig`. Keep the prototype opt-in. The native app is built
in when enabled; no `.nap` ABI additions, partition changes, flash-encryption changes or eFuse operations are required.
Follow the repository's existing installation instructions for the firmware;
the build task does not automatically flash anything.

Host tests need Node 22+ and GCC, not ESP-IDF. They compile the **actual** core
and selected Lua sources, exercise errors and execution limits, and write a
deterministic demo rendering to `build/pico8-host/demo.ppm`.

```sh
SANITIZE=1 npm run pico8:test
```

In ptrace-based environments LeakSanitizer cannot operate; use
`ASAN_OPTIONS=detect_leaks=0` there to run AddressSanitizer/UBSan, and report
that limitation explicitly.

## Validation at prototype creation

- Both enabled and disabled ESP-IDF 5.4.4/ESP32-S3 firmware configurations
  built successfully. The default (disabled) image is **3,177,968 bytes** and
  passes the M5Launcher gate with 33,296 bytes free.
- An enabled ESP-IDF 5.4.4/ESP32-S3 firmware build succeeded, within the existing
  standalone app partition. The smaller M5Launcher update slot is checked
  separately: the prototype currently exceeds the 0x310000-byte launcher
  update slot by about 64 KiB. Do not force an in-place update or change partitions to bypass
  that failure. This is why the experiment is off by default.
  The dependency manager refreshed the starting tree's stale
  manifest resolution locally; no unrelated dependency upgrade is part of
  this change. Rebuilds may refresh that pre-existing lockfile too.
- **36 host tests** passed with the actual core/Lua sources, including an
  AddressSanitizer/UBSan run (LeakSanitizer disabled due to ptrace).
- The demo completed 120 frames with input moving its player under a 32 KiB
  Lua cap. On the 64-bit host its Lua allocation peak was **22,187 bytes**;
  machine allocation was **33,112 bytes** including the 32 KiB RAM.
  These are host allocator payloads, not device heap totals or timings.
- Manifest validation passed. The gzip gate reports the same 416 missing
  generated `.gz` files as the starting tree; no gzip debt is added here.
- Launcher capacity/tree tests: **387 passed**, with the new app below the
  registry limit. Icon parity passed. The existing UI lint has unrelated
  violations in baseline apps; this app adds none.
- **No physical Cardputer test has been performed.** RAM fragmentation,
  allocator overhead, task stack and game speed still require device testing.

## Next milestone

Run the demo on an original Cardputer, capture the serial memory/frame reports
and app-task stack high-water mark, then assess a small public-cart corpus.
Add compatibility deliberately from failing carts/tests. Save data, audio and
PICO-8 syntax/numeric behavior are separate follow-up work; Celeste and broad
compatibility are not claimed by this prototype.
