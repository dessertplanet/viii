/*
 * monome_protocol.h — single-header monome serial protocol library
 *
 * Supports grid (all sizes) and arc devices.
 * Platform-agnostic: no I/O, no allocation, no dependencies beyond <stdint.h>.
 *
 * Usage:
 *   In ONE .c file, before including:
 *     #define MONOME_PROTOCOL_IMPLEMENTATION
 *     #include "monome_protocol.h"
 *
 *   In all other files, just:
 *     #include "monome_protocol.h"
 *
 * Configuration (define before including):
 *   MONOME_MAX_GRID_WIDTH   max grid columns  (default: 16)
 *   MONOME_MAX_GRID_HEIGHT  max grid rows     (default: 16)
 *   MONOME_MAX_ENCODERS     max arc encoders  (default: 4)
 *   MONOME_RING_LEDS        LEDs per ring     (fixed: 64)
 *
 * Protocol reference:
 *   https://monome.org/docs/serialosc/serial/
 *
 * The protocol is asymmetric:
 *   Host  -> Device:  LED/ring commands, system queries
 *   Device -> Host:   key/encoder events, system responses
 *
 * Parsing:
 *   Two parse functions handle the two directions independently,
 *   since some command bytes (0x00, 0x01) have different lengths
 *   depending on who sent them.
 *
 *     monome_host_parse()    — a HOST calls this on data FROM a device
 *     monome_device_parse()  — a DEVICE calls this on data FROM a host
 *
 *   Both invoke a user callback for each complete message.
 *
 * Encoding:
 *   monome_encode_*() functions write protocol bytes into a caller-
 *   provided buffer and return the number of bytes written.
 *   Use whichever functions match your role.
 *
 * MIT License — see bottom of file.
 */

#ifndef MONOME_PROTOCOL_H
#define MONOME_PROTOCOL_H

#include <stdint.h>
#include <string.h>

#ifndef MONOME_MAX_GRID_WIDTH
#define MONOME_MAX_GRID_WIDTH 16
#endif

#ifndef MONOME_MAX_GRID_HEIGHT
#define MONOME_MAX_GRID_HEIGHT 16
#endif

#ifndef MONOME_MAX_ENCODERS
#define MONOME_MAX_ENCODERS 4
#endif

#define MONOME_RING_LEDS 64

#define MONOME_MAX_GRID_LEDS \
  (MONOME_MAX_GRID_WIDTH * MONOME_MAX_GRID_HEIGHT)

/* ----------------------------------------------------------------
 * Protocol constants
 * ---------------------------------------------------------------- */

typedef enum {
  /* system (host -> device) */
  MONOME_CMD_QUERY           = 0x00,
  MONOME_CMD_QUERY_ID        = 0x01,
  MONOME_CMD_SET_ID          = 0x02,
  MONOME_CMD_GET_GRID_SIZE   = 0x05,
  MONOME_CMD_SET_GRID_SIZE   = 0x06,
  MONOME_CMD_GET_GRID_OFFSET = 0x07,
  MONOME_CMD_SET_GRID_OFFSET = 0x08,
  MONOME_CMD_GET_ENC_COUNT   = 0x09,
  MONOME_CMD_SET_ENC_COUNT   = 0x0A,

  /* system (device -> host) — same bytes, different payload lengths */
  MONOME_RSP_QUERY           = 0x00,
  MONOME_RSP_QUERY_ID        = 0x01,
  MONOME_RSP_GRID_SIZE       = 0x03,
  MONOME_RSP_GRID_OFFSET     = 0x04,
  MONOME_RSP_ENC_COUNT       = 0x0B,

  /* grid LED  (host -> device) */
  MONOME_GRID_LED_OFF        = 0x10,
  MONOME_GRID_LED_ON         = 0x11,
  MONOME_GRID_LED_ALL_OFF    = 0x12,
  MONOME_GRID_LED_ALL_ON     = 0x13,
  MONOME_GRID_LED_MAP        = 0x14,
  MONOME_GRID_LED_ROW        = 0x15,
  MONOME_GRID_LED_COL        = 0x16,
  MONOME_GRID_LED_INTENSITY  = 0x17,
  MONOME_GRID_LED_LEVEL_SET  = 0x18,
  MONOME_GRID_LED_LEVEL_ALL  = 0x19,
  MONOME_GRID_LED_LEVEL_MAP  = 0x1A,
  MONOME_GRID_LED_LEVEL_ROW  = 0x1B,
  MONOME_GRID_LED_LEVEL_COL  = 0x1C,

  /* grid key (device -> host) */
  MONOME_GRID_KEY_UP         = 0x20,
  MONOME_GRID_KEY_DOWN       = 0x21,

  /* tilt (device -> host) */
  MONOME_TILT_DATA           = 0x60,

  /* tilt activate (host -> device) */
  MONOME_TILT_SET            = 0x70,

  /* encoder (device -> host) */
  MONOME_ENC_DELTA           = 0x50,
  MONOME_ENC_KEY             = 0x51,

  /* ring LED (host -> device) */
  MONOME_RING_SET            = 0x90,
  MONOME_RING_ALL            = 0x91,
  MONOME_RING_MAP            = 0x92,
  MONOME_RING_RANGE          = 0x93,
} monome_cmd_t;

/* ----------------------------------------------------------------
 * Events — tagged union produced by the parser
 * ---------------------------------------------------------------- */

typedef enum {
  /* system */
  MONOME_EVENT_QUERY_RESPONSE,     /* device capability report       */
  MONOME_EVENT_ID_RESPONSE,        /* device id string               */
  MONOME_EVENT_GRID_SIZE_RESPONSE, /* grid width/height              */
  MONOME_EVENT_GRID_OFFSET,        /* grid offset                    */
  MONOME_EVENT_ENC_COUNT_RESPONSE, /* encoder count                  */
  MONOME_EVENT_QUERY,              /* host requests capabilities     */
  MONOME_EVENT_QUERY_ID,           /* host requests id               */
  MONOME_EVENT_SET_ID,             /* host sets id                   */
  MONOME_EVENT_GET_GRID_SIZE,      /* host requests grid size        */
  MONOME_EVENT_SET_GRID_SIZE,      /* host sets grid size            */
  MONOME_EVENT_GET_GRID_OFFSET,    /* host requests grid offset      */
  MONOME_EVENT_SET_GRID_OFFSET,    /* host sets grid offset          */
  MONOME_EVENT_GET_ENC_COUNT,      /* host requests encoder count    */
  MONOME_EVENT_SET_ENC_COUNT,      /* host sets encoder count        */

  /* grid key */
  MONOME_EVENT_GRID_KEY,           /* key press/release (x, y, z)    */

  /* grid LED (1-bit commands normalized to levels 0/15) */
  MONOME_EVENT_GRID_LED_SET,       /* single LED level               */
  MONOME_EVENT_GRID_LED_ALL,       /* all LEDs to one level          */
  MONOME_EVENT_GRID_LED_MAP,       /* 8x8 quadrant with levels       */
  MONOME_EVENT_GRID_LED_ROW,       /* row with levels                */
  MONOME_EVENT_GRID_LED_COL,       /* column with levels             */
  MONOME_EVENT_GRID_LED_INTENSITY, /* global intensity               */

  /* arc encoder */
  MONOME_EVENT_ENC_DELTA,          /* encoder turned (n, delta)      */
  MONOME_EVENT_ENC_KEY,            /* encoder pressed (n, z)         */

  /* arc ring LED */
  MONOME_EVENT_RING_SET,           /* single ring LED                */
  MONOME_EVENT_RING_ALL,           /* all LEDs in one ring           */
  MONOME_EVENT_RING_MAP,           /* full ring map (64 levels)      */
  MONOME_EVENT_RING_RANGE,         /* range of ring LEDs             */

  /* tilt */
  MONOME_EVENT_TILT,               /* tilt sensor data               */
  MONOME_EVENT_TILT_SET,           /* tilt activate/deactivate       */
} monome_event_type_t;

/* capability types reported in query response */
typedef enum {
  MONOME_DEVICE_LED_GRID = 1,
  MONOME_DEVICE_KEY_GRID = 2,
  MONOME_DEVICE_DIGITAL_OUT = 3,
  MONOME_DEVICE_DIGITAL_IN = 4,
  MONOME_DEVICE_ENCODER = 5,
  MONOME_DEVICE_ANALOG_IN = 6,
  MONOME_DEVICE_ANALOG_OUT = 7,
  MONOME_DEVICE_TILT = 8,
  MONOME_DEVICE_LED_RING = 9,
} monome_capability_type_t;

typedef struct {
  monome_event_type_t type;
  union {
    /* grid key */
    struct { uint8_t x, y, z; } grid_key;

    /* grid LED on/off */
    struct { uint8_t x, y, level; } grid_led_set;

    /* grid LED all */
    struct { uint8_t level; } grid_led_all;

    /* grid LED map — 8x8 quadrant with varibright levels */
    struct { uint8_t x_off, y_off; uint8_t levels[64]; } grid_led_map;

    /* grid LED row */
    struct { uint8_t x_off, y; uint8_t levels[8]; } grid_led_row;

    /* grid LED col */
    struct { uint8_t x, y_off; uint8_t levels[8]; } grid_led_col;

    /* grid LED intensity */
    struct { uint8_t intensity; } grid_led_intensity;

    /* encoder delta */
    struct { uint8_t n; int8_t delta; } enc_delta;

    /* encoder key */
    struct { uint8_t n, z; } enc_key;

    /* ring set */
    struct { uint8_t n, pos, level; } ring_set;

    /* ring all */
    struct { uint8_t n, level; } ring_all;

    /* ring map — all 64 LEDs in one ring */
    struct { uint8_t n; uint8_t levels[MONOME_RING_LEDS]; } ring_map;

    /* ring range */
    struct { uint8_t n, start, end, level; } ring_range;

    /* tilt data */
    struct { uint8_t n; int16_t x, y, z; } tilt;

    /* tilt activate */
    struct { uint8_t n, active; } tilt_set;

    /* system: query response (one capability) */
    struct { uint8_t device_type, count; } query_response;

    /* system: id response / set id */
    struct { char id[33]; } id;

    /* system: grid size */
    struct { uint8_t x, y; } grid_size;

    /* system: grid offset */
    struct { uint8_t x, y; } grid_offset;

    /* system: encoder count */
    struct { uint8_t count; } enc_count;
  } data;
} monome_event_t;

/* ----------------------------------------------------------------
 * Parser state
 * ---------------------------------------------------------------- */

typedef void (*monome_event_callback_t)(
    const monome_event_t *event, void *userdata);

#define MONOME_PARSE_BUF_SIZE 35

typedef struct {
  uint8_t buf[MONOME_PARSE_BUF_SIZE];
  uint8_t len;              /* bytes currently in buf    */
  uint8_t expected;         /* bytes needed for current message (0 = waiting for cmd byte) */
  monome_event_callback_t callback;
  void *userdata;
} monome_parser_t;

/* ----------------------------------------------------------------
 * API — parser
 * ---------------------------------------------------------------- */

/* Initialize a parser. callback is invoked for each complete message. */
void monome_parser_init(monome_parser_t *p,
                        monome_event_callback_t callback,
                        void *userdata);

/* Reset parser state (e.g. after disconnect). */
void monome_parser_reset(monome_parser_t *p);

/*
 * Feed bytes from a DEVICE into this parser (host-side).
 * Parses key events, encoder deltas, query responses, etc.
 *
 * Returns the number of bytes consumed (may be less than len if the
 * stream contains an unrecognized command byte — remaining bytes are
 * discarded up to the next recognized command).
 */
uint32_t monome_host_parse(monome_parser_t *p,
                           const uint8_t *data, uint32_t len);

/*
 * Feed bytes from a HOST into this parser (device-side).
 * Parses LED commands, ring commands, system queries, etc.
 */
uint32_t monome_device_parse(monome_parser_t *p,
                             const uint8_t *data, uint32_t len);

/* ----------------------------------------------------------------
 * API — message encoding
 *
 * All encode functions write into buf[] and return the byte count.
 * Caller must ensure buf is large enough (sizes noted per function).
 *
 * These are pure functions with no side effects.
 * ---------------------------------------------------------------- */

/* --- system queries (host -> device) --- */

/* [0x00] — 1 byte */
uint32_t monome_encode_query(uint8_t *buf);

/* [0x01] — 1 byte */
uint32_t monome_encode_query_id(uint8_t *buf);

/* [0x05] — 1 byte */
uint32_t monome_encode_get_grid_size(uint8_t *buf);

/* [0x09] — 1 byte */
uint32_t monome_encode_get_enc_count(uint8_t *buf);

/* [0x07] — 1 byte */
uint32_t monome_encode_get_grid_offset(uint8_t *buf);

/* [0x02, id[32]] — 33 bytes */
uint32_t monome_encode_set_id(uint8_t *buf, const char *id);

/* [0x06, x, y] — 3 bytes */
uint32_t monome_encode_set_grid_size(uint8_t *buf, uint8_t x, uint8_t y);

/* [0x08, x, y] — 3 bytes */
uint32_t monome_encode_set_grid_offset(uint8_t *buf, uint8_t x, uint8_t y);

/* [0x0A, count] — 2 bytes */
uint32_t monome_encode_set_enc_count(uint8_t *buf, uint8_t count);

/* --- system responses (device -> host) --- */

/* [0x00, type, count] — 3 bytes per capability (call once per capability) */
uint32_t monome_encode_query_response(uint8_t *buf, uint8_t device_type,
                                      uint8_t count);

/* [0x01, id[32]] — 33 bytes */
uint32_t monome_encode_id_response(uint8_t *buf, const char *id);

/* [0x03, x, y] — 3 bytes */
uint32_t monome_encode_grid_size_response(uint8_t *buf, uint8_t x, uint8_t y);

/* [0x04, x, y] — 3 bytes */
uint32_t monome_encode_grid_offset_response(uint8_t *buf, uint8_t x,
                                            uint8_t y);

/* [0x0B, count] — 2 bytes */
uint32_t monome_encode_enc_count_response(uint8_t *buf, uint8_t count);

/* --- grid key events (device -> host) --- */

/* [0x20|0x21, x, y] — 3 bytes.  z: 0 = up, nonzero = down */
uint32_t monome_encode_grid_key(uint8_t *buf, uint8_t x, uint8_t y,
                                uint8_t z);

/* --- grid 1-bit LED commands (host -> device) --- */

/* [0x10, x, y] — 3 bytes */
uint32_t monome_encode_grid_led_off(uint8_t *buf, uint8_t x, uint8_t y);

/* [0x11, x, y] — 3 bytes */
uint32_t monome_encode_grid_led_on(uint8_t *buf, uint8_t x, uint8_t y);

/* [0x12] — 1 byte */
uint32_t monome_encode_grid_led_all_off(uint8_t *buf);

/* [0x13] — 1 byte */
uint32_t monome_encode_grid_led_all_on(uint8_t *buf);

/* [0x14, x_off, y_off, d[8]] — 11 bytes.
 * d: 8 bytes, each is a bitmask for one row of an 8x8 quadrant. */
uint32_t monome_encode_grid_led_map(uint8_t *buf, uint8_t x_off,
                                    uint8_t y_off, const uint8_t bitmask[8]);

/* [0x15, x_off, y, d] — 4 bytes.  d: bitmask for 8 columns. */
uint32_t monome_encode_grid_led_row(uint8_t *buf, uint8_t x_off, uint8_t y,
                                    uint8_t bitmask);

/* [0x16, x, y_off, d] — 4 bytes.  d: bitmask for 8 rows. */
uint32_t monome_encode_grid_led_col(uint8_t *buf, uint8_t x, uint8_t y_off,
                                    uint8_t bitmask);

/* [0x17, level] — 2 bytes */
uint32_t monome_encode_grid_led_intensity(uint8_t *buf, uint8_t intensity);

/* --- grid varibright LED commands (host -> device) --- */

/* [0x18, x, y, level] — 4 bytes.  level: 0-15 */
uint32_t monome_encode_grid_led_level_set(uint8_t *buf, uint8_t x, uint8_t y,
                                          uint8_t level);

/* [0x19, level] — 2 bytes */
uint32_t monome_encode_grid_led_level_all(uint8_t *buf, uint8_t level);

/*
 * [0x1A, x_off, y_off, d[32]] — 35 bytes.
 *
 * levels[64]: row-major 8x8 quadrant, 4-bit levels (0-15).
 * Packed: two levels per byte, high nibble first.
 */
uint32_t monome_encode_grid_led_level_map(uint8_t *buf, uint8_t x_off,
                                          uint8_t y_off,
                                          const uint8_t levels[64]);

/*
 * [0x1B, x_off, y, d[4]] — 7 bytes.
 * levels[8]: 8 consecutive columns, 4-bit each.  Packed into 4 bytes.
 */
uint32_t monome_encode_grid_led_level_row(uint8_t *buf, uint8_t x_off,
                                          uint8_t y,
                                          const uint8_t levels[8]);

/*
 * [0x1C, x, y_off, d[4]] — 7 bytes.
 * levels[8]: 8 consecutive rows, 4-bit each.  Packed into 4 bytes.
 */
uint32_t monome_encode_grid_led_level_col(uint8_t *buf, uint8_t x,
                                          uint8_t y_off,
                                          const uint8_t levels[8]);

/* --- tilt (host -> device) --- */

/* [0x70, n, active] — 3 bytes */
uint32_t monome_encode_tilt_set(uint8_t *buf, uint8_t n, uint8_t active);

/* --- tilt data (device -> host) --- */

/* [0x60, n, xH, xL, yH, yL, zH, zL] — 8 bytes */
uint32_t monome_encode_tilt(uint8_t *buf, uint8_t n, int16_t x, int16_t y,
                            int16_t z);

/* --- encoder events (device -> host) --- */

/* [0x50, n, delta] — 3 bytes.  delta: signed (-128..127) */
uint32_t monome_encode_enc_delta(uint8_t *buf, uint8_t n, int8_t delta);

/* [0x51, n, z] — 3 bytes */
uint32_t monome_encode_enc_key(uint8_t *buf, uint8_t n, uint8_t z);

/* --- ring LED commands (host -> device) --- */

/* [0x90, n, pos, level] — 4 bytes.  pos: 0-63, level: 0-15 */
uint32_t monome_encode_ring_set(uint8_t *buf, uint8_t n, uint8_t pos,
                                uint8_t level);

/* [0x91, n, level] — 3 bytes */
uint32_t monome_encode_ring_all(uint8_t *buf, uint8_t n, uint8_t level);

/*
 * [0x92, n, d[32]] — 34 bytes.
 * levels[64]: all 64 LED positions, 0-15.
 * Packed: two levels per byte, high nibble first.
 */
uint32_t monome_encode_ring_map(uint8_t *buf, uint8_t n,
                                const uint8_t levels[64]);

/* [0x93, n, start, end, level] — 5 bytes.  start/end: 0-63, wraps. */
uint32_t monome_encode_ring_range(uint8_t *buf, uint8_t n, uint8_t start,
                                  uint8_t end, uint8_t level);

/* ----------------------------------------------------------------
 * Optional: grid state helper
 *
 * A convenience struct that tracks LED and key state for one grid.
 * Operations update internal state and produce encoded messages
 * ready to send.
 * ---------------------------------------------------------------- */

typedef struct {
  uint8_t size_x, size_y;
  uint8_t led[MONOME_MAX_GRID_LEDS];  /* varibright levels 0-15 */
  uint8_t key[MONOME_MAX_GRID_LEDS];  /* 1 = currently pressed  */
  uint8_t intensity;
  char id[33];
} monome_grid_state_t;

void monome_grid_state_init(monome_grid_state_t *g, uint8_t sx, uint8_t sy);
void monome_grid_state_clear(monome_grid_state_t *g);

/* Apply a parsed event to the grid state (useful for both sides). */
void monome_grid_state_apply(monome_grid_state_t *g,
                             const monome_event_t *event);

typedef struct {
  uint8_t enc_count;
  uint8_t led[MONOME_MAX_ENCODERS][MONOME_RING_LEDS]; /* levels 0-15 */
  char id[33];
} monome_arc_state_t;

void monome_arc_state_init(monome_arc_state_t *a, uint8_t enc_count);
void monome_arc_state_clear(monome_arc_state_t *a);
void monome_arc_state_apply(monome_arc_state_t *a,
                            const monome_event_t *event);

#endif /* MONOME_PROTOCOL_H */

/* ================================================================
 * IMPLEMENTATION
 * ================================================================ */

#ifdef MONOME_PROTOCOL_IMPLEMENTATION
#ifndef MONOME_PROTOCOL_IMPLEMENTATION_GUARD
#define MONOME_PROTOCOL_IMPLEMENTATION_GUARD

/* ----------------------------------------------------------------
 * Internal: message length tables
 * ---------------------------------------------------------------- */

/*
 * Message lengths for data coming FROM a device (parsed by host).
 * Indexed by command byte.  0 = unrecognized.
 */
static uint8_t monome__device_msg_len(uint8_t cmd) {
  switch (cmd) {
  case 0x00: return 3;  /* query response: cmd, type, count      */
  case 0x01: return 33; /* id response: cmd + 32 chars           */
  case 0x03: return 3;  /* grid size: cmd, x, y                  */
  case 0x04: return 3;  /* grid offset: cmd, x, y                */
  case 0x0B: return 2;  /* enc count: cmd, count                 */
  case 0x20: return 3;  /* key up: cmd, x, y                     */
  case 0x21: return 3;  /* key down: cmd, x, y                   */
  case 0x50: return 3;  /* enc delta: cmd, n, delta              */
  case 0x51: return 3;  /* enc key: cmd, n, z                    */
  case 0x60: return 8;  /* tilt: cmd, n, xH, xL, yH, yL, zH, zL */
  default:   return 0;
  }
}

/*
 * Message lengths for data coming FROM a host (parsed by device).
 * Indexed by command byte.  0 = unrecognized.
 */
static uint8_t monome__host_msg_len(uint8_t cmd) {
  switch (cmd) {
  case 0x00: return 1;  /* query                                 */
  case 0x01: return 1;  /* query id                              */
  case 0x02: return 33; /* set id: cmd + 32 chars                */
  case 0x05: return 1;  /* get grid size                         */
  case 0x06: return 3;  /* set grid size: cmd, x, y              */
  case 0x07: return 1;  /* get grid offset                       */
  case 0x08: return 3;  /* set grid offset: cmd, x, y            */
  case 0x09: return 1;  /* get enc count                         */
  case 0x0A: return 2;  /* set enc count: cmd, count             */
  case 0x10: return 3;  /* led off: cmd, x, y                    */
  case 0x11: return 3;  /* led on: cmd, x, y                     */
  case 0x12: return 1;  /* all off                               */
  case 0x13: return 1;  /* all on                                */
  case 0x14: return 11; /* led map: cmd, xo, yo, d[8]            */
  case 0x15: return 4;  /* led row: cmd, xo, y, d                */
  case 0x16: return 4;  /* led col: cmd, x, yo, d                */
  case 0x17: return 2;  /* intensity: cmd, level                 */
  case 0x18: return 4;  /* level set: cmd, x, y, level           */
  case 0x19: return 2;  /* level all: cmd, level                 */
  case 0x1A: return 35; /* level map: cmd, xo, yo, d[32]         */
  case 0x1B: return 7;  /* level row: cmd, xo, y, d[4]           */
  case 0x1C: return 7;  /* level col: cmd, x, yo, d[4]           */
  case 0x70: return 3;  /* tilt set: cmd, n, active              */
  case 0x90: return 4;  /* ring set: cmd, n, pos, level          */
  case 0x91: return 3;  /* ring all: cmd, n, level               */
  case 0x92: return 34; /* ring map: cmd, n, d[32]               */
  case 0x93: return 5;  /* ring range: cmd, n, start, end, level */
  default:   return 0;
  }
}

/* ----------------------------------------------------------------
 * Internal: unpack helpers
 * ---------------------------------------------------------------- */

/*
 * Unpack packed 4-bit nibbles into an array of 8-bit levels.
 * packed: N bytes -> out: N*2 levels (high nibble first).
 */
static void monome__unpack_nibbles(const uint8_t *packed, uint8_t *out,
                                   uint32_t packed_len) {
  for (uint32_t i = 0; i < packed_len; i++) {
    out[i * 2]     = (packed[i] >> 4) & 0x0F;
    out[i * 2 + 1] = packed[i] & 0x0F;
  }
}

/*
 * Unpack a 1-bit bitmask byte into 8 levels (0 or 15).
 */
static void monome__unpack_bits(uint8_t bits, uint8_t *out) {
  for (uint8_t i = 0; i < 8; i++) {
    out[i] = ((bits >> i) & 1) ? 15 : 0;
  }
}

/* ----------------------------------------------------------------
 * Parser
 * ---------------------------------------------------------------- */

void monome_parser_init(monome_parser_t *p,
                        monome_event_callback_t callback,
                        void *userdata) {
  memset(p, 0, sizeof(*p));
  p->callback = callback;
  p->userdata = userdata;
}

void monome_parser_reset(monome_parser_t *p) {
  p->len = 0;
  p->expected = 0;
}

/*
 * Internal dispatch: decode a complete message buffer into an event
 * and invoke the callback.  direction: 0 = from device, 1 = from host.
 */
static void monome__dispatch(monome_parser_t *p, int from_host) {
  monome_event_t ev;
  memset(&ev, 0, sizeof(ev));
  uint8_t *b = p->buf;

  if (from_host) {
    /* ----- messages from host (device is parsing) ----- */
    switch (b[0]) {

    /* system */
    case 0x00: ev.type = MONOME_EVENT_QUERY; break;
    case 0x01: ev.type = MONOME_EVENT_QUERY_ID; break;
    case 0x02:
      ev.type = MONOME_EVENT_SET_ID;
      memcpy(ev.data.id.id, &b[1], 32);
      ev.data.id.id[32] = '\0';
      break;
    case 0x05: ev.type = MONOME_EVENT_GET_GRID_SIZE; break;
    case 0x06:
      ev.type = MONOME_EVENT_SET_GRID_SIZE;
      ev.data.grid_size.x = b[1];
      ev.data.grid_size.y = b[2];
      break;
    case 0x07: ev.type = MONOME_EVENT_GET_GRID_OFFSET; break;
    case 0x08:
      ev.type = MONOME_EVENT_SET_GRID_OFFSET;
      ev.data.grid_offset.x = b[1];
      ev.data.grid_offset.y = b[2];
      break;
    case 0x09: ev.type = MONOME_EVENT_GET_ENC_COUNT; break;
    case 0x0A:
      ev.type = MONOME_EVENT_SET_ENC_COUNT;
      ev.data.enc_count.count = b[1];
      break;

    /* grid 1-bit LED */
    case 0x10:
      ev.type = MONOME_EVENT_GRID_LED_SET;
      ev.data.grid_led_set.x = b[1];
      ev.data.grid_led_set.y = b[2];
      ev.data.grid_led_set.level = 0;
      break;
    case 0x11:
      ev.type = MONOME_EVENT_GRID_LED_SET;
      ev.data.grid_led_set.x = b[1];
      ev.data.grid_led_set.y = b[2];
      ev.data.grid_led_set.level = 15;
      break;
    case 0x12:
      ev.type = MONOME_EVENT_GRID_LED_ALL;
      ev.data.grid_led_all.level = 0;
      break;
    case 0x13:
      ev.type = MONOME_EVENT_GRID_LED_ALL;
      ev.data.grid_led_all.level = 15;
      break;
    case 0x14:
      ev.type = MONOME_EVENT_GRID_LED_MAP;
      ev.data.grid_led_map.x_off = b[1];
      ev.data.grid_led_map.y_off = b[2];
      for (uint8_t row = 0; row < 8; row++)
        monome__unpack_bits(b[3 + row], &ev.data.grid_led_map.levels[row * 8]);
      break;
    case 0x15:
      ev.type = MONOME_EVENT_GRID_LED_ROW;
      ev.data.grid_led_row.x_off = b[1];
      ev.data.grid_led_row.y = b[2];
      monome__unpack_bits(b[3], ev.data.grid_led_row.levels);
      break;
    case 0x16:
      ev.type = MONOME_EVENT_GRID_LED_COL;
      ev.data.grid_led_col.x = b[1];
      ev.data.grid_led_col.y_off = b[2];
      monome__unpack_bits(b[3], ev.data.grid_led_col.levels);
      break;
    case 0x17:
      ev.type = MONOME_EVENT_GRID_LED_INTENSITY;
      ev.data.grid_led_intensity.intensity = b[1];
      break;

    /* grid varibright LED (same event types as 1-bit, with actual level) */
    case 0x18:
      ev.type = MONOME_EVENT_GRID_LED_SET;
      ev.data.grid_led_set.x = b[1];
      ev.data.grid_led_set.y = b[2];
      ev.data.grid_led_set.level = b[3];
      break;
    case 0x19:
      ev.type = MONOME_EVENT_GRID_LED_ALL;
      ev.data.grid_led_all.level = b[1];
      break;
    case 0x1A:
      ev.type = MONOME_EVENT_GRID_LED_MAP;
      ev.data.grid_led_map.x_off = b[1];
      ev.data.grid_led_map.y_off = b[2];
      monome__unpack_nibbles(&b[3], ev.data.grid_led_map.levels, 32);
      break;
    case 0x1B:
      ev.type = MONOME_EVENT_GRID_LED_ROW;
      ev.data.grid_led_row.x_off = b[1];
      ev.data.grid_led_row.y = b[2];
      monome__unpack_nibbles(&b[3], ev.data.grid_led_row.levels, 4);
      break;
    case 0x1C:
      ev.type = MONOME_EVENT_GRID_LED_COL;
      ev.data.grid_led_col.x = b[1];
      ev.data.grid_led_col.y_off = b[2];
      monome__unpack_nibbles(&b[3], ev.data.grid_led_col.levels, 4);
      break;

    /* tilt activate */
    case 0x70:
      ev.type = MONOME_EVENT_TILT_SET;
      ev.data.tilt_set.n = b[1];
      ev.data.tilt_set.active = b[2];
      break;

    /* ring LED */
    case 0x90:
      ev.type = MONOME_EVENT_RING_SET;
      ev.data.ring_set.n = b[1];
      ev.data.ring_set.pos = b[2];
      ev.data.ring_set.level = b[3];
      break;
    case 0x91:
      ev.type = MONOME_EVENT_RING_ALL;
      ev.data.ring_all.n = b[1];
      ev.data.ring_all.level = b[2];
      break;
    case 0x92:
      ev.type = MONOME_EVENT_RING_MAP;
      ev.data.ring_map.n = b[1];
      monome__unpack_nibbles(&b[2], ev.data.ring_map.levels, 32);
      break;
    case 0x93:
      ev.type = MONOME_EVENT_RING_RANGE;
      ev.data.ring_range.n = b[1];
      ev.data.ring_range.start = b[2];
      ev.data.ring_range.end = b[3];
      ev.data.ring_range.level = b[4];
      break;

    default: return; /* unknown — don't callback */
    }
  } else {
    /* ----- messages from device (host is parsing) ----- */
    switch (b[0]) {

    /* system responses */
    case 0x00:
      ev.type = MONOME_EVENT_QUERY_RESPONSE;
      ev.data.query_response.device_type = b[1];
      ev.data.query_response.count = b[2];
      break;
    case 0x01:
      ev.type = MONOME_EVENT_ID_RESPONSE;
      memcpy(ev.data.id.id, &b[1], 32);
      ev.data.id.id[32] = '\0';
      break;
    case 0x03:
      ev.type = MONOME_EVENT_GRID_SIZE_RESPONSE;
      ev.data.grid_size.x = b[1];
      ev.data.grid_size.y = b[2];
      break;
    case 0x04:
      ev.type = MONOME_EVENT_GRID_OFFSET;
      ev.data.grid_offset.x = b[1];
      ev.data.grid_offset.y = b[2];
      break;
    case 0x0B:
      ev.type = MONOME_EVENT_ENC_COUNT_RESPONSE;
      ev.data.enc_count.count = b[1];
      break;

    /* grid key */
    case 0x20:
      ev.type = MONOME_EVENT_GRID_KEY;
      ev.data.grid_key.x = b[1];
      ev.data.grid_key.y = b[2];
      ev.data.grid_key.z = 0;
      break;
    case 0x21:
      ev.type = MONOME_EVENT_GRID_KEY;
      ev.data.grid_key.x = b[1];
      ev.data.grid_key.y = b[2];
      ev.data.grid_key.z = 1;
      break;

    /* encoder */
    case 0x50:
      ev.type = MONOME_EVENT_ENC_DELTA;
      ev.data.enc_delta.n = b[1];
      ev.data.enc_delta.delta = (int8_t)b[2];
      break;
    case 0x51:
      ev.type = MONOME_EVENT_ENC_KEY;
      ev.data.enc_key.n = b[1];
      ev.data.enc_key.z = b[2];
      break;

    /* tilt */
    case 0x60:
      ev.type = MONOME_EVENT_TILT;
      ev.data.tilt.n = b[1];
      ev.data.tilt.x = (int16_t)((b[2] << 8) | b[3]);
      ev.data.tilt.y = (int16_t)((b[4] << 8) | b[5]);
      ev.data.tilt.z = (int16_t)((b[6] << 8) | b[7]);
      break;

    default: return;
    }
  }

  if (p->callback)
    p->callback(&ev, p->userdata);
}

/*
 * Generic parse loop.  get_len returns the expected message length
 * for a given command byte (0 = unknown).
 */
static uint32_t monome__parse(monome_parser_t *p, const uint8_t *data,
                              uint32_t len,
                              uint8_t (*get_len)(uint8_t),
                              int from_host) {
  uint32_t consumed = 0;
  for (uint32_t i = 0; i < len; i++) {
    if (p->len == 0) {
      /* waiting for command byte */
      uint8_t msg_len = get_len(data[i]);
      if (msg_len == 0) {
        /* unrecognized command — skip byte */
        consumed++;
        continue;
      }
      p->expected = msg_len;
      p->buf[0] = data[i];
      p->len = 1;
      if (p->expected == 1) {
        /* single-byte command — dispatch immediately */
        monome__dispatch(p, from_host);
        p->len = 0;
        p->expected = 0;
      }
    } else {
      /* accumulating payload */
      if (p->len < MONOME_PARSE_BUF_SIZE) {
        p->buf[p->len] = data[i];
      }
      p->len++;
      if (p->len >= p->expected) {
        monome__dispatch(p, from_host);
        p->len = 0;
        p->expected = 0;
      }
    }
    consumed++;
  }
  return consumed;
}

uint32_t monome_host_parse(monome_parser_t *p, const uint8_t *data,
                           uint32_t len) {
  return monome__parse(p, data, len, monome__device_msg_len, 0);
}

uint32_t monome_device_parse(monome_parser_t *p, const uint8_t *data,
                             uint32_t len) {
  return monome__parse(p, data, len, monome__host_msg_len, 1);
}

/* ----------------------------------------------------------------
 * Encoding: system
 * ---------------------------------------------------------------- */

uint32_t monome_encode_query(uint8_t *buf) {
  buf[0] = 0x00; return 1;
}

uint32_t monome_encode_query_id(uint8_t *buf) {
  buf[0] = 0x01; return 1;
}

uint32_t monome_encode_get_grid_size(uint8_t *buf) {
  buf[0] = 0x05; return 1;
}

uint32_t monome_encode_get_enc_count(uint8_t *buf) {
  buf[0] = 0x09; return 1;
}

uint32_t monome_encode_get_grid_offset(uint8_t *buf) {
  buf[0] = 0x07; return 1;
}

uint32_t monome_encode_set_id(uint8_t *buf, const char *id) {
  buf[0] = 0x02;
  memset(&buf[1], 0, 32);
  size_t len = strlen(id);
  if (len > 32) len = 32;
  memcpy(&buf[1], id, len);
  return 33;
}

uint32_t monome_encode_set_grid_size(uint8_t *buf, uint8_t x, uint8_t y) {
  buf[0] = 0x06; buf[1] = x; buf[2] = y; return 3;
}

uint32_t monome_encode_set_grid_offset(uint8_t *buf, uint8_t x, uint8_t y) {
  buf[0] = 0x08; buf[1] = x; buf[2] = y; return 3;
}

uint32_t monome_encode_set_enc_count(uint8_t *buf, uint8_t count) {
  buf[0] = 0x0A; buf[1] = count; return 2;
}

/* ----------------------------------------------------------------
 * Encoding: system responses
 * ---------------------------------------------------------------- */

uint32_t monome_encode_query_response(uint8_t *buf, uint8_t device_type,
                                      uint8_t count) {
  buf[0] = 0x00; buf[1] = device_type; buf[2] = count; return 3;
}

uint32_t monome_encode_id_response(uint8_t *buf, const char *id) {
  buf[0] = 0x01;
  memset(&buf[1], 0, 32);
  size_t len = strlen(id);
  if (len > 32) len = 32;
  memcpy(&buf[1], id, len);
  return 33;
}

uint32_t monome_encode_grid_size_response(uint8_t *buf, uint8_t x,
                                          uint8_t y) {
  buf[0] = 0x03; buf[1] = x; buf[2] = y; return 3;
}

uint32_t monome_encode_grid_offset_response(uint8_t *buf, uint8_t x,
                                            uint8_t y) {
  buf[0] = 0x04; buf[1] = x; buf[2] = y; return 3;
}

uint32_t monome_encode_enc_count_response(uint8_t *buf, uint8_t count) {
  buf[0] = 0x0B; buf[1] = count; return 2;
}

/* ----------------------------------------------------------------
 * Encoding: grid key events
 * ---------------------------------------------------------------- */

uint32_t monome_encode_grid_key(uint8_t *buf, uint8_t x, uint8_t y,
                                uint8_t z) {
  buf[0] = z ? 0x21 : 0x20;
  buf[1] = x;
  buf[2] = y;
  return 3;
}

/* ----------------------------------------------------------------
 * Encoding: grid 1-bit LED
 * ---------------------------------------------------------------- */

uint32_t monome_encode_grid_led_off(uint8_t *buf, uint8_t x, uint8_t y) {
  buf[0] = 0x10; buf[1] = x; buf[2] = y; return 3;
}

uint32_t monome_encode_grid_led_on(uint8_t *buf, uint8_t x, uint8_t y) {
  buf[0] = 0x11; buf[1] = x; buf[2] = y; return 3;
}

uint32_t monome_encode_grid_led_all_off(uint8_t *buf) {
  buf[0] = 0x12; return 1;
}

uint32_t monome_encode_grid_led_all_on(uint8_t *buf) {
  buf[0] = 0x13; return 1;
}

uint32_t monome_encode_grid_led_map(uint8_t *buf, uint8_t x_off,
                                    uint8_t y_off,
                                    const uint8_t bitmask[8]) {
  buf[0] = 0x14;
  buf[1] = x_off;
  buf[2] = y_off;
  memcpy(&buf[3], bitmask, 8);
  return 11;
}

uint32_t monome_encode_grid_led_row(uint8_t *buf, uint8_t x_off, uint8_t y,
                                    uint8_t bitmask) {
  buf[0] = 0x15; buf[1] = x_off; buf[2] = y; buf[3] = bitmask; return 4;
}

uint32_t monome_encode_grid_led_col(uint8_t *buf, uint8_t x, uint8_t y_off,
                                    uint8_t bitmask) {
  buf[0] = 0x16; buf[1] = x; buf[2] = y_off; buf[3] = bitmask; return 4;
}

uint32_t monome_encode_grid_led_intensity(uint8_t *buf, uint8_t intensity) {
  buf[0] = 0x17; buf[1] = intensity; return 2;
}

/* ----------------------------------------------------------------
 * Encoding: grid varibright LED
 * ---------------------------------------------------------------- */

uint32_t monome_encode_grid_led_level_set(uint8_t *buf, uint8_t x, uint8_t y,
                                          uint8_t level) {
  buf[0] = 0x18; buf[1] = x; buf[2] = y; buf[3] = level; return 4;
}

uint32_t monome_encode_grid_led_level_all(uint8_t *buf, uint8_t level) {
  buf[0] = 0x19; buf[1] = level; return 2;
}

uint32_t monome_encode_grid_led_level_map(uint8_t *buf, uint8_t x_off,
                                          uint8_t y_off,
                                          const uint8_t levels[64]) {
  buf[0] = 0x1A;
  buf[1] = x_off;
  buf[2] = y_off;
  for (uint32_t i = 0; i < 32; i++) {
    buf[3 + i] = (uint8_t)((levels[i * 2] << 4) | (levels[i * 2 + 1] & 0x0F));
  }
  return 35;
}

uint32_t monome_encode_grid_led_level_row(uint8_t *buf, uint8_t x_off,
                                          uint8_t y,
                                          const uint8_t levels[8]) {
  buf[0] = 0x1B;
  buf[1] = x_off;
  buf[2] = y;
  for (uint32_t i = 0; i < 4; i++) {
    buf[3 + i] = (uint8_t)((levels[i * 2] << 4) | (levels[i * 2 + 1] & 0x0F));
  }
  return 7;
}

uint32_t monome_encode_grid_led_level_col(uint8_t *buf, uint8_t x,
                                          uint8_t y_off,
                                          const uint8_t levels[8]) {
  buf[0] = 0x1C;
  buf[1] = x;
  buf[2] = y_off;
  for (uint32_t i = 0; i < 4; i++) {
    buf[3 + i] = (uint8_t)((levels[i * 2] << 4) | (levels[i * 2 + 1] & 0x0F));
  }
  return 7;
}

/* ----------------------------------------------------------------
 * Encoding: tilt
 * ---------------------------------------------------------------- */

uint32_t monome_encode_tilt_set(uint8_t *buf, uint8_t n, uint8_t active) {
  buf[0] = 0x70; buf[1] = n; buf[2] = active; return 3;
}

uint32_t monome_encode_tilt(uint8_t *buf, uint8_t n, int16_t x, int16_t y,
                            int16_t z) {
  buf[0] = 0x60;
  buf[1] = n;
  buf[2] = (uint8_t)(x >> 8);
  buf[3] = (uint8_t)(x & 0xFF);
  buf[4] = (uint8_t)(y >> 8);
  buf[5] = (uint8_t)(y & 0xFF);
  buf[6] = (uint8_t)(z >> 8);
  buf[7] = (uint8_t)(z & 0xFF);
  return 8;
}

/* ----------------------------------------------------------------
 * Encoding: encoder events
 * ---------------------------------------------------------------- */

uint32_t monome_encode_enc_delta(uint8_t *buf, uint8_t n, int8_t delta) {
  buf[0] = 0x50; buf[1] = n; buf[2] = (uint8_t)delta; return 3;
}

uint32_t monome_encode_enc_key(uint8_t *buf, uint8_t n, uint8_t z) {
  buf[0] = 0x51; buf[1] = n; buf[2] = z; return 3;
}

/* ----------------------------------------------------------------
 * Encoding: ring LED
 * ---------------------------------------------------------------- */

uint32_t monome_encode_ring_set(uint8_t *buf, uint8_t n, uint8_t pos,
                                uint8_t level) {
  buf[0] = 0x90; buf[1] = n; buf[2] = pos; buf[3] = level; return 4;
}

uint32_t monome_encode_ring_all(uint8_t *buf, uint8_t n, uint8_t level) {
  buf[0] = 0x91; buf[1] = n; buf[2] = level; return 3;
}

uint32_t monome_encode_ring_map(uint8_t *buf, uint8_t n,
                                const uint8_t levels[64]) {
  buf[0] = 0x92;
  buf[1] = n;
  for (uint32_t i = 0; i < 32; i++) {
    buf[2 + i] = (uint8_t)((levels[i * 2] << 4) | (levels[i * 2 + 1] & 0x0F));
  }
  return 34;
}

uint32_t monome_encode_ring_range(uint8_t *buf, uint8_t n, uint8_t start,
                                  uint8_t end, uint8_t level) {
  buf[0] = 0x93; buf[1] = n; buf[2] = start; buf[3] = end; buf[4] = level;
  return 5;
}

/* ----------------------------------------------------------------
 * Grid state helper
 * ---------------------------------------------------------------- */

void monome_grid_state_init(monome_grid_state_t *g, uint8_t sx, uint8_t sy) {
  memset(g, 0, sizeof(*g));
  g->size_x = sx;
  g->size_y = sy;
  g->intensity = 15;
}

void monome_grid_state_clear(monome_grid_state_t *g) {
  memset(g->led, 0, sizeof(g->led));
  memset(g->key, 0, sizeof(g->key));
}

void monome_grid_state_apply(monome_grid_state_t *g,
                             const monome_event_t *event) {
  uint16_t sx = g->size_x;
  switch (event->type) {

  case MONOME_EVENT_GRID_KEY:
    if (event->data.grid_key.x < g->size_x &&
        event->data.grid_key.y < g->size_y) {
      g->key[event->data.grid_key.y * sx + event->data.grid_key.x] =
          event->data.grid_key.z;
    }
    break;

  case MONOME_EVENT_GRID_LED_SET:
    if (event->data.grid_led_set.x < g->size_x &&
        event->data.grid_led_set.y < g->size_y) {
      g->led[event->data.grid_led_set.y * sx + event->data.grid_led_set.x] =
          event->data.grid_led_set.level;
    }
    break;

  case MONOME_EVENT_GRID_LED_ALL:
    memset(g->led, event->data.grid_led_all.level,
           g->size_x * g->size_y);
    break;

  case MONOME_EVENT_GRID_LED_MAP: {
    uint8_t xo = event->data.grid_led_map.x_off;
    uint8_t yo = event->data.grid_led_map.y_off;
    for (uint8_t row = 0; row < 8; row++) {
      if ((yo + row) >= g->size_y) break;
      for (uint8_t col = 0; col < 8; col++) {
        if ((xo + col) >= g->size_x) break;
        g->led[(yo + row) * sx + xo + col] =
            event->data.grid_led_map.levels[row * 8 + col];
      }
    }
    break;
  }

  case MONOME_EVENT_GRID_LED_ROW: {
    uint8_t xo = event->data.grid_led_row.x_off;
    uint8_t y  = event->data.grid_led_row.y;
    if (y >= g->size_y) break;
    for (uint8_t col = 0; col < 8; col++) {
      if ((xo + col) >= g->size_x) break;
      g->led[y * sx + xo + col] = event->data.grid_led_row.levels[col];
    }
    break;
  }

  case MONOME_EVENT_GRID_LED_COL: {
    uint8_t x  = event->data.grid_led_col.x;
    uint8_t yo = event->data.grid_led_col.y_off;
    if (x >= g->size_x) break;
    for (uint8_t row = 0; row < 8; row++) {
      if ((yo + row) >= g->size_y) break;
      g->led[(yo + row) * sx + x] = event->data.grid_led_col.levels[row];
    }
    break;
  }

  case MONOME_EVENT_GRID_LED_INTENSITY:
    g->intensity = event->data.grid_led_intensity.intensity;
    break;

  case MONOME_EVENT_GRID_SIZE_RESPONSE:
    g->size_x = event->data.grid_size.x;
    g->size_y = event->data.grid_size.y;
    break;

  case MONOME_EVENT_ID_RESPONSE:
    memcpy(g->id, event->data.id.id, 33);
    break;

  default:
    break;
  }
}

/* ----------------------------------------------------------------
 * Arc state helper
 * ---------------------------------------------------------------- */

void monome_arc_state_init(monome_arc_state_t *a, uint8_t enc_count) {
  memset(a, 0, sizeof(*a));
  a->enc_count = enc_count;
}

void monome_arc_state_clear(monome_arc_state_t *a) {
  memset(a->led, 0, sizeof(a->led));
}

void monome_arc_state_apply(monome_arc_state_t *a,
                            const monome_event_t *event) {
  switch (event->type) {

  case MONOME_EVENT_RING_SET:
    if (event->data.ring_set.n < a->enc_count &&
        event->data.ring_set.pos < MONOME_RING_LEDS) {
      a->led[event->data.ring_set.n][event->data.ring_set.pos] =
          event->data.ring_set.level;
    }
    break;

  case MONOME_EVENT_RING_ALL:
    if (event->data.ring_all.n < a->enc_count) {
      memset(a->led[event->data.ring_all.n], event->data.ring_all.level,
             MONOME_RING_LEDS);
    }
    break;

  case MONOME_EVENT_RING_MAP:
    if (event->data.ring_map.n < a->enc_count) {
      memcpy(a->led[event->data.ring_map.n], event->data.ring_map.levels,
             MONOME_RING_LEDS);
    }
    break;

  case MONOME_EVENT_RING_RANGE:
    if (event->data.ring_range.n < a->enc_count) {
      uint8_t n = event->data.ring_range.n;
      uint8_t s = event->data.ring_range.start;
      uint8_t e = event->data.ring_range.end;
      uint8_t lv = event->data.ring_range.level;
      if (s <= e) {
        for (uint8_t i = s; i <= e && i < MONOME_RING_LEDS; i++)
          a->led[n][i] = lv;
      } else {
        /* wraps around */
        for (uint8_t i = s; i < MONOME_RING_LEDS; i++)
          a->led[n][i] = lv;
        for (uint8_t i = 0; i <= e && i < MONOME_RING_LEDS; i++)
          a->led[n][i] = lv;
      }
    }
    break;

  case MONOME_EVENT_ENC_COUNT_RESPONSE:
    a->enc_count = event->data.enc_count.count;
    break;

  case MONOME_EVENT_ID_RESPONSE:
    memcpy(a->id, event->data.id.id, 33);
    break;

  default:
    break;
  }
}

#endif /* MONOME_PROTOCOL_IMPLEMENTATION_GUARD */
#endif /* MONOME_PROTOCOL_IMPLEMENTATION */

/*
 * MIT License
 *
 * Copyright (c) 2026 monome
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
