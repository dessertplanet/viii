/*
 * midi_web.c — midi.h implementation for browser
 *
 * MIDI TX goes to the host via WebMIDI (JS bridge).
 * MIDI RX arrives from JS via viii_midi_rx() when a WebMIDI
 * input message is received.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <emscripten.h>

#include "midi.h"

/* ----------------------------------------------------------------
 * JS bridge — send MIDI to host via WebMIDI
 * ---------------------------------------------------------------- */

EM_JS(void, js_midi_tx, (uint8_t d1, uint8_t d2, uint8_t d3), {
  if (Module.onMidiTx) {
    Module.onMidiTx(d1, d2, d3);
  }
});

/* ----------------------------------------------------------------
 * RX buffer — filled by viii_midi_rx() from JS
 * ---------------------------------------------------------------- */

#define MIDI_RX_QUEUE_SIZE 64

static struct {
  uint8_t d1, d2, d3;
} midi_rx_queue[MIDI_RX_QUEUE_SIZE];

static uint8_t midi_rx_w = 0;
static uint8_t midi_rx_r = 0;

/* ----------------------------------------------------------------
 * Callback
 * ---------------------------------------------------------------- */

static void midi_rx_noop(uint8_t d1, uint8_t d2, uint8_t d3) {
  (void)d1; (void)d2; (void)d3;
}

static midi_rx_callback_t rx_callback = &midi_rx_noop;

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

void midi_task(void) {
  while (midi_rx_r != midi_rx_w) {
    rx_callback(
      midi_rx_queue[midi_rx_r].d1,
      midi_rx_queue[midi_rx_r].d2,
      midi_rx_queue[midi_rx_r].d3
    );
    midi_rx_r = (midi_rx_r + 1) % MIDI_RX_QUEUE_SIZE;
  }
}

void midi_tx(uint8_t data1, uint8_t data2, uint8_t data3) {
  js_midi_tx(data1, data2, data3);
}

void midi_set_rx_callback(midi_rx_callback_t callback) {
  rx_callback = callback;
}

/* ----------------------------------------------------------------
 * Called from JS when a WebMIDI message arrives
 * ---------------------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE
void viii_midi_rx(uint8_t data1, uint8_t data2, uint8_t data3) {
  uint8_t next = (midi_rx_w + 1) % MIDI_RX_QUEUE_SIZE;
  if (next != midi_rx_r) {
    midi_rx_queue[midi_rx_w].d1 = data1;
    midi_rx_queue[midi_rx_w].d2 = data2;
    midi_rx_queue[midi_rx_w].d3 = data3;
    midi_rx_w = next;
  }
}
