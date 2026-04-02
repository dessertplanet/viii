/*
 * device_web.c — device.h implementation for browser
 *
 * Bridges the Lua grid API to a physical monome grid over WebSerial
 * using libmonome mext protocol elements. LED commands from Lua are batched in a
 * local buffer and sent as LEVEL_MAP quadrants on refresh. Key events
 * from the grid are parsed and dispatched to the Lua VM.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>

#include "lua.h"
#include "lauxlib.h"
#include <monome.h>

#include "device.h"
#include "vm.h"
#include "serial.h"

/* EMBED_PROTOS build exports this constructor from libmonome mext.c */
extern monome_t *monome_protocol_mext_new(void);
typedef struct monome_devmap monome_devmap_t;

/* ----------------------------------------------------------------
 * JS bridge — send monome protocol bytes to WebSerial
 * ---------------------------------------------------------------- */

EM_JS(void, js_monome_tx, (const uint8_t *data, uint32_t len), {
  if (Module.onMonomeTx) {
    var bytes = new Uint8Array(HEAPU8.buffer, data, len).slice();
    Module.onMonomeTx(bytes);
  }
});

/* ----------------------------------------------------------------
 * Grid state
 * ---------------------------------------------------------------- */

#define MAX_GRID_X 16
#define MAX_GRID_Y 16

static uint8_t grid_size_x_val = 16;
static uint8_t grid_size_y_val = 8;
static uint8_t grid_led[MAX_GRID_X * MAX_GRID_Y];
static uint8_t grid_intensity_val = 15;
static bool grid_intensity_pending = false;
static bool grid_refresh_pending = false;
static bool grid_connected = false;
static bool grid_dirty[4] = {false, false, false, false};

/* ---- arc state ---- */
#define MAX_ENCODERS 4
#define RING_LEDS 64

static uint8_t arc_enc_count = 0;
static uint8_t arc_ring[MAX_ENCODERS][RING_LEDS];
static uint8_t arc_intensity_val = 15;
static uint16_t arc_res_val[MAX_ENCODERS]; /* ticks per delta (1 = raw) */
static int32_t arc_accum[MAX_ENCODERS];    /* accumulator for resolution */
static bool arc_refresh_pending = false;
static bool arc_intensity_pending = false;
static bool device_is_arc = false;

/* key event queue */
#define KEY_QUEUE_SIZE 128

static struct {
  uint8_t x, y, z;
} key_queue[KEY_QUEUE_SIZE];

static uint8_t key_queue_w = 0;
static uint8_t key_queue_r = 0;

/* encoder event queue */
#define ENC_QUEUE_SIZE 128

static struct {
  uint8_t type; /* 0 = delta, 1 = key */
  uint8_t n;
  int8_t delta;
  uint8_t z;
} enc_queue[ENC_QUEUE_SIZE];

static uint8_t enc_queue_w = 0;
static uint8_t enc_queue_r = 0;

/* libmonome device instance (mext protocol) */
static monome_t *grid_monome = NULL;

/* incoming bytes buffered for libmonome platform_read() */
#define MONOME_RX_BUFSIZE 8192
static uint8_t monome_rx_buf[MONOME_RX_BUFSIZE];
static uint32_t monome_rx_w = 0;
static uint32_t monome_rx_r = 0;

/* lightweight mext system-message sniffer for discovery responses
 * (grid size + encoder count) that libmonome parses but doesn't expose */
static uint8_t sniffer_buf[64];
static uint8_t sniffer_len = 0;
static uint8_t sniffer_expected = 0;

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

static inline int clamp_val(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint8_t grid_quad_idx(uint8_t x, uint8_t y) {
  return ((y >= 8) << 1) | (x >= 8);
}

static inline uint32_t monome_rx_available(void) {
  return monome_rx_w - monome_rx_r;
}

/* mext device→host response lengths (header byte → total message length) */
static uint8_t mext_response_len(uint8_t header) {
  uint8_t addr = header >> 4;
  uint8_t cmd = header & 0x0F;
  switch (addr) {
    case 0x0: /* system */
      switch (cmd) {
        case 0x0: return 3;  /* query response: header + subsystem + count */
        case 0x1: return 33; /* id */
        case 0x2: return 4;  /* grid offset */
        case 0x3: return 3;  /* grid size: header + cols + rows */
        case 0x4: return 3;  /* addr */
        case 0xF: return 9;  /* version */
        default: return 1;
      }
    case 0x2: return (cmd <= 0x1) ? 3 : 1; /* key grid */
    case 0x5: /* encoder */
      if (cmd == 0x0) return 3;
      if (cmd == 0x1 || cmd == 0x2) return 2;
      return 1;
    case 0x8: /* tilt */
      if (cmd == 0x0) return 2;
      if (cmd == 0x1) return 8;
      return 1;
    default: return 1;
  }
}

/* sniff mext system responses for grid size + encoder count */
static void sniffer_consume_byte(uint8_t byte) {
  if (sniffer_expected == 0) {
    /* new message header */
    sniffer_buf[0] = byte;
    sniffer_len = 1;
    sniffer_expected = mext_response_len(byte);
    if (sniffer_expected <= 1) {
      sniffer_expected = 0;
      sniffer_len = 0;
    }
    return;
  }

  if (sniffer_len < sizeof(sniffer_buf)) {
    sniffer_buf[sniffer_len++] = byte;
  }

  if (sniffer_len >= sniffer_expected) {
    uint8_t header = sniffer_buf[0];
    uint8_t addr = header >> 4;
    uint8_t cmd = header & 0x0F;

    if (addr == 0x0) {
      if (cmd == 0x3 && sniffer_expected == 3) {
        /* grid size response: [0x03, cols, rows] */
        uint8_t cols = sniffer_buf[1];
        uint8_t rows = sniffer_buf[2];
        if (cols > 0 && cols <= MAX_GRID_X) grid_size_x_val = cols;
        if (rows > 0 && rows <= MAX_GRID_Y) grid_size_y_val = rows;
      } else if (cmd == 0x0 && sniffer_expected == 3) {
        /* query response: [0x00, subsystem, count] */
        uint8_t subsystem = sniffer_buf[1];
        uint8_t count = sniffer_buf[2];
        if (subsystem == 0x5) { /* encoder subsystem */
          arc_enc_count = count > MAX_ENCODERS ? MAX_ENCODERS : count;
          if (arc_enc_count > 0) device_is_arc = true;
        }
      }
    }

    sniffer_expected = 0;
    sniffer_len = 0;
  }
}

static void grid_send(const uint8_t *data, uint32_t len) {
  js_monome_tx(data, len);
}

char *monome_platform_get_dev_serial(const char *device) {
  (void)device;
  return NULL;
}

monome_t *monome_platform_load_protocol(const char *proto) {
  if (proto && strcmp(proto, "mext") == 0) {
    return monome_protocol_mext_new();
  }
  return NULL;
}

void monome_platform_free(monome_t *monome) {
  free(monome);
}

int monome_platform_open(monome_t *monome, const monome_devmap_t *m, const char *dev) {
  (void)monome;
  (void)m;
  (void)dev;
  return 0;
}

int monome_platform_close(monome_t *monome) {
  (void)monome;
  return 0;
}

ssize_t monome_platform_write(monome_t *monome, const uint8_t *buf, size_t nbyte) {
  (void)monome;
  if (!grid_connected || !buf || nbyte == 0) return 0;
  js_monome_tx(buf, (uint32_t)nbyte);
  return (ssize_t)nbyte;
}

ssize_t monome_platform_read(monome_t *monome, uint8_t *buf, size_t nbyte) {
  (void)monome;
  uint32_t avail = monome_rx_available();
  if (!buf || nbyte == 0 || avail == 0) return 0;

  size_t to_read = nbyte < avail ? nbyte : (size_t)avail;
  memcpy(buf, monome_rx_buf + monome_rx_r, to_read);
  monome_rx_r += (uint32_t)to_read;

  if (monome_rx_r == monome_rx_w) {
    monome_rx_r = 0;
    monome_rx_w = 0;
  }

  return (ssize_t)to_read;
}

int monome_platform_wait_for_input(monome_t *monome, unsigned int msec) {
  (void)monome;
  (void)msec;
  return monome_rx_available() > 0 ? 1 : 0;
}

void *m_malloc(size_t size) {
  return malloc(size);
}

void *m_calloc(size_t nmemb, size_t size) {
  return calloc(nmemb, size);
}

void *m_strdup(const char *s) {
  if (!s) return NULL;
  size_t len = strlen(s);
  char *out = malloc(len + 1);
  if (!out) return NULL;
  memcpy(out, s, len + 1);
  return out;
}

void m_free(void *ptr) {
  free(ptr);
}

void m_sleep(unsigned int msec) {
  (void)msec;
}

/* ----------------------------------------------------------------
 * Direct mext byte construction for LED output.
 * Matches ansible's grid_map_mext / ring_map_mext byte-for-byte.
 * Used for all devices — simpler and avoids libmonome struct-packing
 * issues under emscripten/WASM.  libmonome is still used for
 * incoming event parsing (RX).
 * ---------------------------------------------------------------- */

/* hex-dump TX bytes to JS console (for debugging, normally off) */
static bool grid_tx_debug = false;

EM_JS(void, js_grid_tx_hexdump, (const uint8_t *data, uint32_t len), {
  if (!Module._gridTxDebug) return;
  var hex = [];
  for (var i = 0; i < len; i++) {
    hex.push(('0' + HEAPU8[data + i].toString(16)).slice(-2));
  }
  console.log('monome TX (' + len + '): ' + hex.join(' '));
});

EMSCRIPTEN_KEEPALIVE
void viii_set_tx_debug(uint8_t enabled) {
  grid_tx_debug = enabled != 0;
  /* stash a JS-side flag so the EM_JS hexdump can check cheaply */
  EM_ASM({ Module._gridTxDebug = $0; }, (int)grid_tx_debug);
}

/* send raw mext bytes to grid (with optional hex dump).
 * Always pads to 64 bytes with 0xFF. */
static void grid_send_raw(const uint8_t *data, uint32_t len) {
  js_grid_tx_hexdump(data, len);

  if (len < 64) {
    uint8_t padded[64];
    memcpy(padded, data, len);
    memset(padded + len, 0xFF, 64 - len);
    js_monome_tx(padded, 64);
  } else {
    js_monome_tx(data, len);
  }
}

/* LEVEL_MAP: 0x1A <x> <y> <32 packed-nybble bytes>  (35 total) */
static void raw_level_map(uint8_t x, uint8_t y, const uint8_t *levels) {
  uint8_t buf[35];
  buf[0] = 0x1A;
  buf[1] = x;
  buf[2] = y;

  uint8_t *p = buf + 3;
  for (uint8_t r = 0; r < 8; r++) {
    for (uint8_t j = 0; j < 4; j++) {
      *p = (levels[0] << 4) | (levels[1] & 0x0F);
      levels += 2;
      p++;
    }
  }
  grid_send_raw(buf, 35);
}

/* RING_MAP: 0x92 <n> <32 packed-nybble bytes>  (34 total) */
static void raw_ring_map(uint8_t n, const uint8_t *levels) {
  uint8_t buf[34];
  buf[0] = 0x92;
  buf[1] = n;

  uint8_t *p = buf + 2;
  for (uint8_t i = 0; i < 32; i++) {
    *p = (levels[0] << 4) | (levels[1] & 0x0F);
    levels += 2;
    p++;
  }
  grid_send_raw(buf, 34);
}

/* LED_INTENSITY: 0x17 <level>  (2 bytes) */
static void raw_led_intensity(uint8_t level) {
  uint8_t buf[2] = { 0x17, level & 0x0F };
  grid_send_raw(buf, 2);
}

/* RING_INTENSITY: 0x94 <level>  (2 bytes) */
static void raw_ring_intensity(uint8_t level) {
  uint8_t buf[2] = { 0x94, level & 0x0F };
  grid_send_raw(buf, 2);
}

/* LED_ALL_OFF: 0x12  (1 byte) */
static void raw_led_all_off(void) {
  uint8_t buf[1] = { 0x12 };
  grid_send_raw(buf, 1);
}

static void grid_send_refresh(void) {
  if (!grid_connected) return;

  if (grid_intensity_pending) {
    raw_led_intensity(grid_intensity_val);
    grid_intensity_pending = false;
  }

  for (uint8_t yo = 0; yo < grid_size_y_val; yo += 8) {
    for (uint8_t xo = 0; xo < grid_size_x_val; xo += 8) {
      uint8_t q = grid_quad_idx(xo, yo);
      if (!grid_dirty[q]) continue;

      uint8_t levels[64];
      for (uint8_t r = 0; r < 8; r++) {
        for (uint8_t c = 0; c < 8; c++) {
          uint8_t gy = yo + r;
          uint8_t gx = xo + c;
          if (gx < grid_size_x_val && gy < grid_size_y_val)
            levels[r * 8 + c] = grid_led[gy * MAX_GRID_X + gx];
          else
            levels[r * 8 + c] = 0;
        }
      }
      raw_level_map(xo, yo, levels);
      grid_dirty[q] = false;
    }
  }
}

static void arc_send_refresh(void) {
  if (!grid_connected) return;

  if (arc_intensity_pending) {
    raw_ring_intensity(arc_intensity_val);
    arc_intensity_pending = false;
  }

  uint8_t ring_count = arc_enc_count;
  if (device_is_arc && ring_count == 0) ring_count = MAX_ENCODERS;

  for (uint8_t n = 0; n < ring_count; n++) {
    raw_ring_map(n, arc_ring[n]);
  }
}

/* ----------------------------------------------------------------
 * libmonome event callbacks — handle messages FROM the grid
 * ---------------------------------------------------------------- */

static void queue_grid_key(uint8_t x, uint8_t y, uint8_t z) {
  uint8_t next = (key_queue_w + 1) % KEY_QUEUE_SIZE;
  if (next == key_queue_r) return;
  key_queue[key_queue_w].x = x;
  key_queue[key_queue_w].y = y;
  key_queue[key_queue_w].z = z;
  key_queue_w = next;
}

static void queue_enc_delta(uint8_t n, int8_t delta) {
  uint8_t next = (enc_queue_w + 1) % ENC_QUEUE_SIZE;
  if (next == enc_queue_r) return;
  enc_queue[enc_queue_w].type = 0;
  enc_queue[enc_queue_w].n = n;
  enc_queue[enc_queue_w].delta = delta;
  enc_queue_w = next;
}

static void queue_enc_key(uint8_t n, uint8_t z) {
  uint8_t next = (enc_queue_w + 1) % ENC_QUEUE_SIZE;
  if (next == enc_queue_r) return;
  enc_queue[enc_queue_w].type = 1;
  enc_queue[enc_queue_w].n = n;
  enc_queue[enc_queue_w].z = z;
  enc_queue_w = next;
}

static void handle_grid_button(uint8_t x, uint8_t y, uint8_t z) {
  if (device_is_arc) queue_enc_key(x, z);
  else queue_grid_key(x, y, z);
}

static void on_button_down(const monome_event_t *event, void *userdata) {
  (void)userdata;
  handle_grid_button((uint8_t)event->grid.x, (uint8_t)event->grid.y, 1);
}

static void on_button_up(const monome_event_t *event, void *userdata) {
  (void)userdata;
  handle_grid_button((uint8_t)event->grid.x, (uint8_t)event->grid.y, 0);
}

static void on_encoder_delta(const monome_event_t *event, void *userdata) {
  (void)userdata;
  queue_enc_delta((uint8_t)event->encoder.number, (int8_t)event->encoder.delta);
}

static void on_encoder_key_down(const monome_event_t *event, void *userdata) {
  (void)userdata;
  queue_enc_key((uint8_t)event->encoder.number, 1);
}

static void on_encoder_key_up(const monome_event_t *event, void *userdata) {
  (void)userdata;
  queue_enc_key((uint8_t)event->encoder.number, 0);
}

static void update_device_dims(void) {
  if (!grid_monome) return;
  int cols = monome_get_cols(grid_monome);
  int rows = monome_get_rows(grid_monome);

  if (cols > 0) {
    if (cols > MAX_GRID_X) cols = MAX_GRID_X;
    grid_size_x_val = (uint8_t)cols;
  }
  if (rows > 0) {
    if (rows > MAX_GRID_Y) rows = MAX_GRID_Y;
    grid_size_y_val = (uint8_t)rows;
  }
}

/* ----------------------------------------------------------------
 * vm_handle_grid_key — dispatch key events to Lua
 * (mirrors the function in iii-grid-one/main.c)
 * ---------------------------------------------------------------- */

void vm_handle_grid_key(uint8_t x, uint8_t y, uint8_t z) {
  if (L == NULL) return;
  lua_getglobal(L, "event_grid");
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  lua_pushinteger(L, x + 1); /* 1-based */
  lua_pushinteger(L, y + 1); /* 1-based */
  lua_pushinteger(L, z);
  l_report(L, docall(L, 3, 0));
}

static void vm_handle_enc_delta(uint8_t n, int8_t delta) {
  if (L == NULL) return;
  if (n >= MAX_ENCODERS) return;

  /* apply resolution (matching iii-arc firmware logic) */
  int8_t d;
  if (arc_res_val[n] <= 1) {
    d = delta;
  } else {
    arc_accum[n] += delta;
    if (abs(arc_accum[n]) >= (int32_t)arc_res_val[n]) {
      int32_t tick = arc_accum[n] / (int32_t)arc_res_val[n];
      arc_accum[n] -= tick * (int32_t)arc_res_val[n];
      d = (int8_t)tick;
    } else {
      return; /* not enough accumulated yet */
    }
  }

  lua_getglobal(L, "event_arc");
  if (lua_isnil(L, -1)) { lua_pop(L, 1); return; }
  lua_pushinteger(L, n + 1); /* 1-based */
  lua_pushinteger(L, d);
  l_report(L, docall(L, 2, 0));
}

static void vm_handle_enc_key(uint8_t n, uint8_t z) {
  (void)n; /* arc has single key, iii API: event_arc_key(z) */
  if (L == NULL) return;
  lua_getglobal(L, "event_arc_key");
  if (lua_isnil(L, -1)) { lua_pop(L, 1); return; }
  lua_pushinteger(L, z);
  l_report(L, docall(L, 1, 0));
}

/* ----------------------------------------------------------------
 * Lua bindings — grid LED functions
 * ---------------------------------------------------------------- */

static int l_grid_led(lua_State *l) {
  uint8_t x = (uint8_t)lua_tointeger(l, 1) - 1; /* 1-based */
  uint8_t y = (uint8_t)lua_tointeger(l, 2) - 1;
  int8_t z = (int8_t)lua_tointeger(l, 3);
  bool rel = lua_toboolean(l, 4);

  if (x >= grid_size_x_val || y >= grid_size_y_val) return 0;

  uint16_t idx = y * MAX_GRID_X + x;
  if (rel) z = (int8_t)clamp_val(z + grid_led[idx], 0, 15);
  else z = (int8_t)clamp_val(z, 0, 15);

  grid_led[idx] = (uint8_t)z;
  grid_dirty[grid_quad_idx(x, y)] = true;
  return 0;
}

static int l_grid_led_get(lua_State *l) {
  uint8_t x = (uint8_t)lua_tointeger(l, 1) - 1;
  uint8_t y = (uint8_t)lua_tointeger(l, 2) - 1;
  x = x % grid_size_x_val;
  y = y % grid_size_y_val;
  lua_pushinteger(l, grid_led[y * MAX_GRID_X + x]);
  return 1;
}

static int l_grid_led_all(lua_State *l) {
  int8_t z = (int8_t)lua_tointeger(l, 1);
  bool rel = lua_toboolean(l, 2);

  if (rel) {
    for (uint8_t y = 0; y < grid_size_y_val; y++) {
      for (uint8_t x = 0; x < grid_size_x_val; x++) {
        uint16_t idx = y * MAX_GRID_X + x;
        int8_t zz = (int8_t)clamp_val(z + grid_led[idx], 0, 15);
        grid_led[idx] = (uint8_t)zz;
      }
    }
  } else {
    z = (int8_t)clamp_val(z, 0, 15);
    for (uint8_t y = 0; y < grid_size_y_val; y++) {
      for (uint8_t x = 0; x < grid_size_x_val; x++) {
        grid_led[y * MAX_GRID_X + x] = (uint8_t)z;
      }
    }
  }
  for (int q = 0; q < 4; q++) grid_dirty[q] = true;
  return 0;
}

static int l_grid_intensity(lua_State *l) {
  uint8_t z = (uint8_t)lua_tointeger(l, 1);
  if (z > 15) z = 15;
  grid_intensity_val = z;
  grid_intensity_pending = true;
  grid_refresh_pending = true;
  return 0;
}

static int l_grid_refresh(lua_State *l) {
  (void)l;
  grid_refresh_pending = true;
  return 0;
}

static int l_grid_size_x(lua_State *l) {
  lua_pushinteger(l, grid_size_x_val);
  return 1;
}

static int l_grid_size_y(lua_State *l) {
  lua_pushinteger(l, grid_size_y_val);
  return 1;
}

/* ----------------------------------------------------------------
 * Lua bindings — arc ring LED functions (matching iii arc API)
 * ---------------------------------------------------------------- */

/* arc_led(n, x, z, rel) — set single LED on ring n */
static int l_arc_led(lua_State *l) {
  uint8_t n = (uint8_t)lua_tointeger(l, 1) - 1;
  uint8_t x = (uint8_t)lua_tointeger(l, 2) - 1;
  int8_t z = (int8_t)lua_tointeger(l, 3);
  bool rel = lua_toboolean(l, 4);
  if (n >= MAX_ENCODERS || x >= RING_LEDS) return 0;
  if (rel) z = (int8_t)clamp_val(z + arc_ring[n][x], 0, 15);
  else z = (int8_t)clamp_val(z, 0, 15);
  arc_ring[n][x] = (uint8_t)z;
  return 0;
}

/* arc_led_ring(n, z, rel) — set all LEDs on ring n */
static int l_arc_led_ring(lua_State *l) {
  uint8_t n = (uint8_t)lua_tointeger(l, 1) - 1;
  int8_t z = (int8_t)lua_tointeger(l, 2);
  bool rel = lua_toboolean(l, 3);
  if (n >= MAX_ENCODERS) return 0;
  if (rel) {
    for (int i = 0; i < RING_LEDS; i++) {
      int8_t zz = (int8_t)clamp_val(z + arc_ring[n][i], 0, 15);
      arc_ring[n][i] = (uint8_t)zz;
    }
  } else {
    z = (int8_t)clamp_val(z, 0, 15);
    memset(arc_ring[n], (uint8_t)z, RING_LEDS);
  }
  return 0;
}

/* arc_led_all(z, rel) — set all LEDs on all rings */
static int l_arc_led_all(lua_State *l) {
  int8_t z = (int8_t)lua_tointeger(l, 1);
  bool rel = lua_toboolean(l, 2);
  for (uint8_t n = 0; n < MAX_ENCODERS; n++) {
    if (rel) {
      for (int i = 0; i < RING_LEDS; i++) {
        int8_t zz = (int8_t)clamp_val(z + arc_ring[n][i], 0, 15);
        arc_ring[n][i] = (uint8_t)zz;
      }
    } else {
      memset(arc_ring[n], (uint8_t)clamp_val(z, 0, 15), RING_LEDS);
    }
  }
  return 0;
}

/* arc_led_get(n, x) — read LED value */
static int l_arc_led_get(lua_State *l) {
  uint8_t n = (uint8_t)lua_tointeger(l, 1) - 1;
  uint8_t x = (uint8_t)lua_tointeger(l, 2) - 1;
  if (n >= MAX_ENCODERS || x >= RING_LEDS) { lua_pushinteger(l, 0); return 1; }
  lua_pushinteger(l, arc_ring[n][x]);
  return 1;
}

/* arc_intensity(b) — set global intensity */
static int l_arc_intensity(lua_State *l) {
  uint8_t b = (uint8_t)lua_tointeger(l, 1);
  if (b > 15) b = 15;
  arc_intensity_val = b;
  arc_intensity_pending = true;
  arc_refresh_pending = true;
  return 0;
}

/* arc_res(n, div) — set encoder resolution (ticks per delta, 1-1024) */
static int l_arc_res(lua_State *l) {
  uint8_t n = (uint8_t)lua_tointeger(l, 1) - 1;
  uint16_t r = (uint16_t)lua_tointeger(l, 2);
  if (n >= MAX_ENCODERS) return 0;
  arc_res_val[n] = (uint16_t)clamp_val(r, 1, 1024);
  arc_accum[n] = 0;
  return 0;
}

static int l_arc_refresh(lua_State *l) {
  (void)l;
  arc_refresh_pending = true;
  return 0;
}

static int l_arc_enc_count(lua_State *l) {
  lua_pushinteger(l, arc_enc_count);
  return 1;
}

/* ----------------------------------------------------------------
 * device.h interface
 * ---------------------------------------------------------------- */

static const struct luaL_Reg dlib[] = {
  {"grid_led",       l_grid_led},
  {"grid_led_get",   l_grid_led_get},
  {"grid_led_all",   l_grid_led_all},
  {"grid_intensity", l_grid_intensity},
  {"grid_refresh",   l_grid_refresh},
  {"grid_size_x",    l_grid_size_x},
  {"grid_size_y",    l_grid_size_y},
  {"arc_led",        l_arc_led},
  {"arc_led_ring",   l_arc_led_ring},
  {"arc_led_all",    l_arc_led_all},
  {"arc_led_get",    l_arc_led_get},
  {"arc_intensity",  l_arc_intensity},
  {"arc_res",        l_arc_res},
  {"arc_refresh",    l_arc_refresh},
  {"arc_enc_count",  l_arc_enc_count},
  {NULL, NULL}
};

const struct luaL_Reg *get_device_lib(void) {
  return dlib;
}

const char *device_help_str =
  "\n"
  "grid\n"
  "  event_grid(x,y,z)\n"
  "  grid_led_all(z,rel)\n"
  "  grid_led(x,y,z,rel)\n"
  "  grid_led_get(x,y)\n"
  "  grid_intensity(z)\n"
  "  grid_refresh()\n"
  "  grid_size_x()\n"
  "  grid_size_y()\n"
  "arc\n"
  "  event_arc(n,d)\n"
  "  event_arc_key(z)\n"
  "  arc_res(n,div)\n"
  "  arc_led(n,x,z,rel)\n"
  "  arc_led_ring(n,z,rel)\n"
  "  arc_led_all(z,rel)\n"
  "  arc_intensity(b)\n"
  "  arc_refresh()\n"
  "  arc_led_get(n,x)\n"
  "  arc_enc_count()\n";

const char *device_help_txt(void) { return device_help_str; }
const char *device_id(void) { return "viii"; }
const char *device_version(void) { return "v0.1.0"; }
const char *device_str1(void) { return "monome"; }
const char *device_str2(void) { return "viii"; }

bool check_device_key(void) { return false; }

void device_init(void) {
  grid_monome = monome_protocol_mext_new();
  if (grid_monome) {
    monome_register_handler(grid_monome, MONOME_BUTTON_DOWN, on_button_down, NULL);
    monome_register_handler(grid_monome, MONOME_BUTTON_UP, on_button_up, NULL);
    monome_register_handler(grid_monome, MONOME_ENCODER_DELTA, on_encoder_delta, NULL);
    monome_register_handler(grid_monome, MONOME_ENCODER_KEY_DOWN, on_encoder_key_down, NULL);
    monome_register_handler(grid_monome, MONOME_ENCODER_KEY_UP, on_encoder_key_up, NULL);
  }
  memset(grid_led, 0, sizeof(grid_led));
  memset(arc_ring, 0, sizeof(arc_ring));
  for (int i = 0; i < MAX_ENCODERS; i++) { arc_res_val[i] = 1; arc_accum[i] = 0; }
  grid_refresh_pending = false;
  arc_refresh_pending = false;
  grid_intensity_pending = false;
  arc_intensity_pending = false;
  for (int q = 0; q < 4; q++) grid_dirty[q] = false;
  device_is_arc = false;
  monome_rx_r = 0;
  monome_rx_w = 0;
  sniffer_len = 0;
  sniffer_expected = 0;
}

void device_task(void) {
  /* dispatch queued key events to Lua */
  while (key_queue_r != key_queue_w) {
    vm_handle_grid_key(
      key_queue[key_queue_r].x,
      key_queue[key_queue_r].y,
      key_queue[key_queue_r].z
    );
    key_queue_r = (key_queue_r + 1) % KEY_QUEUE_SIZE;
  }

  /* dispatch queued encoder events to Lua */
  while (enc_queue_r != enc_queue_w) {
    if (enc_queue[enc_queue_r].type == 0)
      vm_handle_enc_delta(enc_queue[enc_queue_r].n, enc_queue[enc_queue_r].delta);
    else
      vm_handle_enc_key(enc_queue[enc_queue_r].n, enc_queue[enc_queue_r].z);
    enc_queue_r = (enc_queue_r + 1) % ENC_QUEUE_SIZE;
  }

  /* send LED updates to physical grid */
  if (grid_refresh_pending) {
    grid_send_refresh();
    grid_refresh_pending = false;
  }

  /* send ring updates to physical arc */
  if (arc_refresh_pending) {
    arc_send_refresh();
    arc_refresh_pending = false;
  }
}

/* ----------------------------------------------------------------
 * Called from JS when WebSerial receives bytes from the grid
 * ---------------------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE
void viii_grid_rx(const uint8_t *data, uint32_t len) {
  if (!grid_monome || !data || len == 0) return;

  if (len > (MONOME_RX_BUFSIZE - monome_rx_w)) {
    monome_rx_r = 0;
    monome_rx_w = 0;
    sniffer_len = 0;
    sniffer_expected = 0;
    return;
  }

  memcpy(monome_rx_buf + monome_rx_w, data, len);
  monome_rx_w += len;

  for (uint32_t i = 0; i < len; i++) {
    sniffer_consume_byte(data[i]);
  }

  while (monome_event_handle_next(grid_monome) > 0) {
  }

  update_device_dims();
}

/* ----------------------------------------------------------------
 * Called from JS after WebSerial connects to the grid.
 * Sends libmonome-style discovery queries.
 * ---------------------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE
void viii_grid_connect(void) {
  grid_connected = true;

  /* wait for discovery replies */
  grid_size_x_val = 0;
  grid_size_y_val = 0;
  arc_enc_count = 0;
  device_is_arc = false;
  monome_rx_r = 0;
  monome_rx_w = 0;
  sniffer_len = 0;
  sniffer_expected = 0;

  /* clear LED state */
  memset(grid_led, 0, sizeof(grid_led));
  memset(arc_ring, 0, sizeof(arc_ring));
  for (int i = 0; i < MAX_ENCODERS; i++) { arc_res_val[i] = 1; arc_accum[i] = 0; }
  for (int q = 0; q < 4; q++) grid_dirty[q] = false;

  /* send discovery queries (standard mext bytes) */
  {
    const uint8_t query_capabilities[] = {0x00};
    const uint8_t query_id[] = {0x01};
    const uint8_t query_size[] = {0x05};

    grid_send(query_capabilities, sizeof(query_capabilities));
    grid_send(query_id, sizeof(query_id));
    grid_send(query_size, sizeof(query_size));
  }

  raw_led_all_off();
}

EMSCRIPTEN_KEEPALIVE
void viii_grid_disconnect(void) {
  grid_connected = false;
}

EMSCRIPTEN_KEEPALIVE
void viii_set_arc_mode(uint8_t is_arc) {
  device_is_arc = is_arc != 0;
  if (!device_is_arc) {
    arc_enc_count = 0;
  }
}

EMSCRIPTEN_KEEPALIVE
uint8_t viii_grid_size_x(void) {
  return grid_size_x_val;
}

EMSCRIPTEN_KEEPALIVE
uint8_t viii_grid_size_y(void) {
  return grid_size_y_val;
}

EMSCRIPTEN_KEEPALIVE
uint8_t viii_arc_enc_count(void) {
  return arc_enc_count;
}
