if(NOT CONFIG_NUCLEO_PICO8_PROTOTYPE)
    idf_component_register()
    return()
endif()

set(LUA_SOURCES
    lapi.c lauxlib.c lbaselib.c lcode.c lctype.c ldebug.c ldo.c ldump.c
    lfunc.c lgc.c llex.c lmathlib.c lmem.c lobject.c lopcodes.c lparser.c
    lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c lvm.c lzio.c)
list(TRANSFORM LUA_SOURCES PREPEND "vendor/lua/")
idf_component_register(SRCS "nucleo_pico8.c" ${LUA_SOURCES}
    INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "vendor/lua"
    EMBED_TXTFILES "demo/pocket-demo.p8")
# Portable C only: no Lua OS, dynamic-library, readline or process bindings.
target_compile_options(${COMPONENT_LIB} PRIVATE -Os -Wno-unused-function)

# Bound parser/C-call recursion to the shared Cardputer app task stack.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LUAI_MAXCCALLS=40 LUA_32BITS=1)
