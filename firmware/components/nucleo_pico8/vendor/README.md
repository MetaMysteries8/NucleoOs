# Lua provenance

`lua/` contains the C sources and headers from Lua **5.4.8**, except
the standalone interpreter (`lua.c`) and compiler (`luac.c`), which are omitted.

- Author: Lua.org / PUC-Rio.
- Source: https://www.lua.org/ftp/lua-5.4.8.tar.gz
- Archive SHA-256: `4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae`
- License: MIT, preserved in `lua/LICENSE.txt` and the source notices.
- One configuration-only edit: `luaconf.h` wraps its `LUA_32BITS` default in
  `#ifndef`, allowing the build to select 32-bit integers/floats. The diff is
  preserved in `lua32bits.patch`. No other upstream source is modified.

The component CMake file names the linked files explicitly. OS, I/O, dynamic
package loading, debug, coroutine and UTF-8 libraries are not linked. The host
test builds the same selection. `LUAI_MAXCCALLS=40` is supplied by the build to
bound parser/C-call recursion; upstream defaults to 200. The host test also
supplies this setting. Both builds also set `LUA_32BITS=1`. This uses Lua's
documented 32-bit-number mode, not PICO-8's fixed-point numeric semantics.

The PICO API, streamed cartridge reader, tiny font, demo and device frontend
are original code. This prototype does **not** include Lexaloffle's runtime,
FABLE-8's browser engine, FAKE-08 or femto8 code. FABLE-8 Pocket is a project
name, not a claim that all browser-engine behavior has been ported.
