/*
 * main.c — viii WASM entry point
 *
 * Replaces iii.c + the main() from iii-grid-one.
 * Initializes all subsystems and provides the main loop iteration
 * that JS calls via emscripten_set_main_loop or setInterval.
 */

#include <stdbool.h>
#include <emscripten.h>

#include "device.h"
#include "flash.h"
#include "fs.h"
#include "metro.h"
#include "midi.h"
#include "repl.h"
#include "serial.h"
#include "vm.h"

/* flag set by watchdog_reboot shim */
static bool reinit_requested = false;

void viii_request_reinit(void) {
  reinit_requested = true;
}

/* ----------------------------------------------------------------
 * Initialization — called from JS after filesystem preload
 * ---------------------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE
void viii_init(void) {
  flash_init();
  fs_init();
  fs_mount();

  metro_init();
  device_init();
  vm_init(true);
  repl_init();

  midi_set_rx_callback(vm_handle_midi);
  serial_set_rx_callback(repl_handle_bytes);
}

/* ----------------------------------------------------------------
 * Main loop iteration — called ~250 times/sec from JS
 * ---------------------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE
void viii_loop(void) {
  if (reinit_requested) {
    reinit_requested = false;
    vm_deinit();
    vm_init(true);
    repl_init();
    midi_set_rx_callback(vm_handle_midi);
    serial_set_rx_callback(repl_handle_bytes);
    return;
  }

  midi_task();
  metro_task();
  serial_task();
  device_task();
}
