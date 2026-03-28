/*
 * device_web.c — device.h implementation for browser
 *
 * Bridges the Lua grid API to a physical monome grid over WebSerial
 * using monome_protocol.h. LED commands from Lua are batched in a
 * local buffer and sent as LEVEL_MAP quadrants on refresh. Key events
 * from the grid are parsed and dispatched to the Lua VM.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <emscripten.h>

#include "lua.h"
#include "lauxlib.h"

#define MONOME_PROTOCOL_IMPLEMENTATION
#include "monome_protocol.h"

#include "device.h"
#include "vm.h"
#include "serial.h"

/* ----------------------------------------------------------------
 * JS bridge — send monome protocol bytes to WebSerial
 * ---------------------------------------------------------------- */

EM_JS(void, js_grid_tx, (const uint8_t *data, uint32_t len), {
  if (Module.onGridTx) {
    var bytes = new Uint8Array(HEAPU8.buffer, data, len).slice();
    Module.onGridTx(bytes);
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
static bool grid_refresh_pending = false;
static bool grid_connected = false;

/* ---- arc state ---- */
#define MAX_ENCODERS 4
#define RING_LEDS 64

static uint8_t arc_enc_count = 0;
static uint8_t arc_ring[MAX_ENCODERS][RING_LEDS];
static uint8_t arc_intensity_val = 15;
static uint16_t arc_res_val[MAX_ENCODERS]; /* ticks per delta (1 = raw) */
static int32_t arc_accum[MAX_ENCODERS];    /* accumulator for resolution */
static bool arc_refresh_pending = false;

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

/* monome protocol parser for incoming device messages */
static monome_parser_t grid_parser;

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

static inline int clamp_val(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static void grid_send(const uint8_t *data, uint32_t len) {
  js_grid_tx(data, len);
}

static void grid_send_refresh(void) {
  if (!grid_connected) return;

  uint8_t buf[35];

  /* send intensity */
  uint32_t n = monome_encode_grid_led_intensity(buf, grid_intensity_val);
  grid_send(buf, n);

  /* send LED state as LEVEL_MAP quadrants */
  for (uint8_t yo = 0; yo < grid_size_y_val; yo += 8) {
    for (uint8_t xo = 0; xo < grid_size_x_val; xo += 8) {
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
      n = monome_encode_grid_led_level_map(buf, xo, yo, levels);
      grid_send(buf, n);
    }
  }
}

static void arc_send_refresh(void) {
  if (!grid_connected) return;

  uint8_t buf[34];
  for (uint8_t n = 0; n < arc_enc_count; n++) {
    uint32_t len = monome_encode_ring_map(buf, n, arc_ring[n]);
    grid_send(buf, len);
  }
}

/* ----------------------------------------------------------------
 * Protocol parser callback — handles messages FROM the grid
 * ---------------------------------------------------------------- */

static void grid_event_cb(const monome_event_t *event, void *userdata) {
  (void)userdata;

  switch (event->type) {
    case MONOME_EVENT_GRID_KEY: {
      /* on an arc, grid key events are the arc button */
      if (arc_enc_count > 0) {
        uint8_t next = (enc_queue_w + 1) % ENC_QUEUE_SIZE;
        if (next != enc_queue_r) {
          enc_queue[enc_queue_w].type = 1;
          enc_queue[enc_queue_w].n = event->data.grid_key.x;
          enc_queue[enc_queue_w].z = event->data.grid_key.z;
          enc_queue_w = next;
        }
      } else {
        uint8_t next = (key_queue_w + 1) % KEY_QUEUE_SIZE;
        if (next != key_queue_r) {
          key_queue[key_queue_w].x = event->data.grid_key.x;
          key_queue[key_queue_w].y = event->data.grid_key.y;
          key_queue[key_queue_w].z = event->data.grid_key.z;
          key_queue_w = next;
        }
      }
      break;
    }

    case MONOME_EVENT_GRID_SIZE_RESPONSE:
      grid_size_x_val = event->data.grid_size.x;
      grid_size_y_val = event->data.grid_size.y;
      if (grid_size_x_val > MAX_GRID_X) grid_size_x_val = MAX_GRID_X;
      if (grid_size_y_val > MAX_GRID_Y) grid_size_y_val = MAX_GRID_Y;
      break;

    case MONOME_EVENT_ENC_COUNT_RESPONSE:
      arc_enc_count = event->data.enc_count.count;
      if (arc_enc_count > MAX_ENCODERS) arc_enc_count = MAX_ENCODERS;
      break;

    case MONOME_EVENT_ENC_DELTA: {
      uint8_t next = (enc_queue_w + 1) % ENC_QUEUE_SIZE;
      if (next != enc_queue_r) {
        enc_queue[enc_queue_w].type = 0;
        enc_queue[enc_queue_w].n = event->data.enc_delta.n;
        enc_queue[enc_queue_w].delta = event->data.enc_delta.delta;
        enc_queue_w = next;
      }
      break;
    }

    case MONOME_EVENT_ENC_KEY: {
      uint8_t next = (enc_queue_w + 1) % ENC_QUEUE_SIZE;
      if (next != enc_queue_r) {
        enc_queue[enc_queue_w].type = 1;
        enc_queue[enc_queue_w].n = event->data.enc_key.n;
        enc_queue[enc_queue_w].z = event->data.enc_key.z;
        enc_queue_w = next;
      }
      break;
    }

    case MONOME_EVENT_QUERY_RESPONSE:
      /* detect arc encoder count from capability report */
      if (event->data.query_response.device_type == MONOME_DEVICE_ENCODER) {
        arc_enc_count = event->data.query_response.count;
        if (arc_enc_count > MAX_ENCODERS) arc_enc_count = MAX_ENCODERS;
      }
      break;

    case MONOME_EVENT_ID_RESPONSE:
      /* device id received */
      break;

    default:
      break;
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
  return 0;
}

static int l_grid_intensity(lua_State *l) {
  uint8_t z = (uint8_t)lua_tointeger(l, 1);
  if (z > 15) z = 15;
  grid_intensity_val = z;
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
  monome_parser_init(&grid_parser, grid_event_cb, NULL);
  memset(grid_led, 0, sizeof(grid_led));
  memset(arc_ring, 0, sizeof(arc_ring));
  for (int i = 0; i < MAX_ENCODERS; i++) { arc_res_val[i] = 1; arc_accum[i] = 0; }
  grid_refresh_pending = false;
  arc_refresh_pending = false;
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
  monome_host_parse(&grid_parser, data, len);
}

/* ----------------------------------------------------------------
 * Called from JS after WebSerial connects to the grid.
 * Sends query + get_size to discover the grid.
 * ---------------------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE
void viii_grid_connect(void) {
  uint8_t buf[4];
  uint32_t n;

  grid_connected = true;
  monome_parser_reset(&grid_parser);

  /* reset detection state for new device */
  grid_size_x_val = 0;
  grid_size_y_val = 0;
  arc_enc_count = 0;

  /* clear LED state */
  memset(grid_led, 0, sizeof(grid_led));
  memset(arc_ring, 0, sizeof(arc_ring));
  for (int i = 0; i < MAX_ENCODERS; i++) { arc_res_val[i] = 1; arc_accum[i] = 0; }

  /* query capabilities */
  n = monome_encode_query(buf);
  grid_send(buf, n);

  /* query id */
  n = monome_encode_query_id(buf);
  grid_send(buf, n);

  /* query size (grid) */
  n = monome_encode_get_grid_size(buf);
  grid_send(buf, n);

  /* query encoder count (arc) */
  n = monome_encode_get_enc_count(buf);
  grid_send(buf, n);

  /* turn all LEDs off */
  n = monome_encode_grid_led_all_off(buf);
  grid_send(buf, n);
}

EMSCRIPTEN_KEEPALIVE
void viii_grid_disconnect(void) {
  grid_connected = false;
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
