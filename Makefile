# viii — virtual iii
# Compiles the iii framework + Lua VM to WebAssembly via Emscripten

III_SRC = iii
LUA_DIR = lua
LIBMONOME_DIR = libmonome

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

ALL_SOURCES = $(VIII_SOURCES) $(III_SOURCES) $(LUA_SOURCES) $(LIBMONOME_PROTO_SOURCES)

# --- libmonome protocol-only (minimal mext subset) ---
LIBMONOME_PROTO_SOURCES = \
	$(LIBMONOME_DIR)/src/libmonome.c \
	$(LIBMONOME_DIR)/src/rotation.c \
	$(LIBMONOME_DIR)/src/monobright.c \
	$(LIBMONOME_DIR)/src/proto/mext.c

LIBMONOME_PROTO_INCLUDES = \
	-I $(LIBMONOME_DIR)/public \
	-I $(LIBMONOME_DIR)/src/private \
	-I $(LIBMONOME_DIR)/src/proto

LIBMONOME_PROTO_BUILD_DIR = build/libmonome-proto
LIBMONOME_PROTO_OBJECTS = \
	$(LIBMONOME_PROTO_BUILD_DIR)/libmonome.o \
	$(LIBMONOME_PROTO_BUILD_DIR)/rotation.o \
	$(LIBMONOME_PROTO_BUILD_DIR)/monobright.o \
	$(LIBMONOME_PROTO_BUILD_DIR)/mext.o

# shims first so they override pico/hardware headers
INCLUDES = \
	-I src/shims \
	-I src \
	-I . \
	-I $(III_SRC) \
	-I $(III_SRC)/resource \
	-I $(LUA_DIR) \
	-I $(LIBMONOME_DIR)/public \
	-I $(LIBMONOME_DIR)/src/private \
	-I $(LIBMONOME_DIR)/src/proto

CFLAGS = -O2 -Wall -Wno-unused-function -DEMBED_PROTOS

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

.PHONY: all clean serve protocol-only

all: web/viii.js

web/viii.js: $(ALL_SOURCES)
	@mkdir -p web
	emcc $(CFLAGS) $(INCLUDES) $(EMFLAGS) $(ALL_SOURCES) -o $@

clean:
	rm -f web/viii.js web/viii.wasm web/libmonome-protocol.a
	rm -rf $(LIBMONOME_PROTO_BUILD_DIR)

serve:
	cd web && python3 -m http.server 8080

protocol-only: web/libmonome-protocol.a

web/libmonome-protocol.a: $(LIBMONOME_PROTO_OBJECTS)
	@mkdir -p web
	emar rcs $@ $(LIBMONOME_PROTO_OBJECTS)

$(LIBMONOME_PROTO_BUILD_DIR)/libmonome.o: $(LIBMONOME_DIR)/src/libmonome.c
	@mkdir -p $(LIBMONOME_PROTO_BUILD_DIR)
	emcc $(CFLAGS) -DEMBED_PROTOS $(LIBMONOME_PROTO_INCLUDES) -c $< -o $@

$(LIBMONOME_PROTO_BUILD_DIR)/rotation.o: $(LIBMONOME_DIR)/src/rotation.c
	@mkdir -p $(LIBMONOME_PROTO_BUILD_DIR)
	emcc $(CFLAGS) -DEMBED_PROTOS $(LIBMONOME_PROTO_INCLUDES) -c $< -o $@

$(LIBMONOME_PROTO_BUILD_DIR)/monobright.o: $(LIBMONOME_DIR)/src/monobright.c
	@mkdir -p $(LIBMONOME_PROTO_BUILD_DIR)
	emcc $(CFLAGS) -DEMBED_PROTOS $(LIBMONOME_PROTO_INCLUDES) -c $< -o $@

$(LIBMONOME_PROTO_BUILD_DIR)/mext.o: $(LIBMONOME_DIR)/src/proto/mext.c
	@mkdir -p $(LIBMONOME_PROTO_BUILD_DIR)
	emcc $(CFLAGS) -DEMBED_PROTOS $(LIBMONOME_PROTO_INCLUDES) -c $< -o $@
