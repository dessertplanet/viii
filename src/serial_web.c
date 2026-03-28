/*
 * serial_web.c — serial.h implementation for browser
 *
 * Output (serial_tx / serial) goes to a JS callback that renders
 * it in the REPL terminal. Input arrives from JS via viii_serial_rx()
 * and is dispatched to the registered rx_callback (repl_handle_bytes).
 */

#include <stdarg.h>
#include <string.h>
#include <emscripten.h>

#include "serial.h"

/* ----------------------------------------------------------------
 * JS bridge — output to terminal
 * ---------------------------------------------------------------- */

EM_JS(void, js_serial_tx, (const char *data, uint32_t len), {
  if (Module.onSerialTx) {
    Module.onSerialTx(UTF8ToString(data, len));
  }
});

/* ----------------------------------------------------------------
 * TX buffer
 * ---------------------------------------------------------------- */

#define SERIAL_TX_BUFSIZE 16384

static char tx_buf[SERIAL_TX_BUFSIZE];
static uint16_t tx_w = 0;
static uint16_t tx_r = 0;

/* ----------------------------------------------------------------
 * RX buffer — filled by viii_serial_rx() from JS
 * ---------------------------------------------------------------- */

#define SERIAL_RX_BUFSIZE 65536

static uint8_t rx_buf[SERIAL_RX_BUFSIZE];
static uint16_t rx_w = 0;
static uint16_t rx_r = 0;

/* ----------------------------------------------------------------
 * Callbacks
 * ---------------------------------------------------------------- */

static void serial_rx_noop(uint8_t *data, uint32_t len) {
  (void)data; (void)len;
}

static serial_rx_callback_t rx_callback = &serial_rx_noop;

static void line_state_cb_noop(bool dtr, bool rts) {
  (void)dtr; (void)rts;
}

static serial_line_state_callback_t line_state_callback = &line_state_cb_noop;

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

void serial_task(void) {
  /* incoming: dispatch buffered bytes to the REPL */
  if (rx_r != rx_w) {
    uint16_t count = rx_w - rx_r;
    rx_callback(rx_buf + rx_r, count);
    rx_r = 0;
    rx_w = 0;
  }

  /* outgoing: flush TX buffer to JS */
  if (tx_w != tx_r) {
    js_serial_tx(tx_buf + tx_r, tx_w - tx_r);
    tx_r = 0;
    tx_w = 0;
  }
}

void serial_tx(const uint8_t *data, uint32_t len) {
  if ((len + tx_w) < SERIAL_TX_BUFSIZE) {
    memcpy(tx_buf + tx_w, data, len);
    tx_w += (uint16_t)len;
  }
}

void serial_tx_str(const char *str) {
  serial_tx((const uint8_t *)str, strlen(str));
}

int serial(const char *fmt, ...) {
  int l;
  va_list myargs;
  va_start(myargs, fmt);
  l = vsnprintf(tx_buf + tx_w, SERIAL_TX_BUFSIZE - tx_w, fmt, myargs);
  va_end(myargs);
  if (l > 0) tx_w += (uint16_t)l;
  return 0;
}

void serial_set_rx_callback(serial_rx_callback_t callback) {
  rx_callback = callback;
}

void serial_set_line_state_callback(serial_line_state_callback_t callback) {
  line_state_callback = callback;
}

/* ----------------------------------------------------------------
 * Called from JS when the user types into the terminal
 * ---------------------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE
void viii_serial_rx(const uint8_t *data, uint32_t len) {
  for (uint32_t i = 0; i < len && rx_w < SERIAL_RX_BUFSIZE; i++) {
    rx_buf[rx_w++] = data[i];
  }
}
