# viii — virtual iii
# Compiles the iii framework + Lua VM to WebAssembly via Emscripten

III_SRC = iii
LUA_DIR = lua

# --- Lua sources (matching iii-grid-one/lua/CMakeLists.txt) ---
LUA_SOURCES = \
	$(LUA_DIR)/lapi.c \
	$(LUA_DIR)/lauxlib.c \
	$(LUA_DIR)/lbaselib.c \
	$(LUA_DIR)/lcode.c \
	$(LUA_DIR)/lcorolib.c \
	$(LUA_DIR)/lctype.c \
	$(LUA_DIR)/ldblib.c \
	$(LUA_DIR)/ldebug.c \
	$(LUA_DIR)/ldo.c \
	$(LUA_DIR)/ldump.c \
	$(LUA_DIR)/lfunc.c \
	$(LUA_DIR)/lgc.c \
	$(LUA_DIR)/linit.c \
	$(LUA_DIR)/liolib.c \
	$(LUA_DIR)/llex.c \
	$(LUA_DIR)/lmathlib.c \
	$(LUA_DIR)/lmem.c \
	$(LUA_DIR)/loadlib.c \
	$(LUA_DIR)/lobject.c \
	$(LUA_DIR)/lopcodes.c \
	$(LUA_DIR)/loslib.c \
	$(LUA_DIR)/lparser.c \
	$(LUA_DIR)/lstate.c \
	$(LUA_DIR)/lstring.c \
	$(LUA_DIR)/lstrlib.c \
	$(LUA_DIR)/ltable.c \
	$(LUA_DIR)/ltablib.c \
	$(LUA_DIR)/ltm.c \
	$(LUA_DIR)/lundump.c \
	$(LUA_DIR)/lutf8lib.c \
	$(LUA_DIR)/lvm.c \
	$(LUA_DIR)/lzio.c

# --- iii framework sources (compiled with our shims) ---
III_SOURCES = \
	$(III_SRC)/vm.c \
	$(III_SRC)/repl.c \
	$(III_SRC)/util.c \
	$(III_SRC)/resource/lib_lua.c

# --- viii web platform sources ---
VIII_SOURCES = \
	src/main.c \
	src/serial_web.c \
	src/midi_web.c \
	src/metro_web.c \
	src/fs_web.c \
	src/flash_web.c \
	src/device_web.c

ALL_SOURCES = $(VIII_SOURCES) $(III_SOURCES) $(LUA_SOURCES)

# shims first so they override pico/hardware headers
INCLUDES = \
	-I src/shims \
	-I src \
	-I . \
	-I $(III_SRC) \
	-I $(III_SRC)/resource \
	-I $(LUA_DIR)

CFLAGS = -O2 -Wall -Wno-unused-function

EXPORTED_FUNCTIONS = \
	_viii_init,\
	_viii_loop,\
	_viii_serial_rx,\
	_viii_grid_rx,\
	_viii_grid_connect,\
	_viii_grid_disconnect,\
	_viii_set_arc_mode,\
	_viii_set_tx_debug,\
	_viii_grid_size_x,\
	_viii_grid_size_y,\
	_viii_arc_enc_count,\
	_viii_arc_key,\
	_viii_midi_rx,\
	_viii_metro_tick,\
	_viii_fs_preload,\
	_malloc,\
	_free

EXPORTED_RUNTIME = ccall,cwrap,UTF8ToString

EMFLAGS = \
	-s EXPORTED_FUNCTIONS='[$(EXPORTED_FUNCTIONS)]' \
	-s EXPORTED_RUNTIME_METHODS='[$(EXPORTED_RUNTIME)]' \
	-s ALLOW_MEMORY_GROWTH=1 \
	-s INITIAL_MEMORY=4194304 \
	-s STACK_SIZE=65536 \
	-s NO_EXIT_RUNTIME=1 \
	-s MODULARIZE=1 \
	-s EXPORT_NAME=createVIII

.PHONY: all clean serve

all: web/viii.js

web/viii.js: $(ALL_SOURCES)
	@mkdir -p web
	emcc $(CFLAGS) $(INCLUDES) $(EMFLAGS) $(ALL_SOURCES) -o $@

clean:
	rm -f web/viii.js web/viii.wasm

serve:
	cd web && python3 -m http.server 8080
