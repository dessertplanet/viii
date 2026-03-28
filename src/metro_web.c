/*
 * metro_web.c — metro.h implementation for browser
 *
 * Uses JS setInterval for timing. Each metro tick sets a pending flag
 * that metro_task() checks on the main loop iteration. Since JS is
 * single-threaded, no locking is needed.
 *
 * Browser timer minimum is ~4ms. Intervals shorter than that will
 * be clamped with a console warning.
 */

#include <stdbool.h>
#include <string.h>
#include <emscripten.h>

#include "metro.h"
#include "vm.h"

/* ----------------------------------------------------------------
 * JS bridge — start/stop JS timers
 * ---------------------------------------------------------------- */

EM_JS(void, js_metro_start, (int index, double interval_ms), {
  if (Module._timerWorker) {
    Module._timerWorker.postMessage({
      type: 'startMetro', index: index, intervalMs: interval_ms
    });
  }
});

EM_JS(void, js_metro_stop, (int index), {
  if (Module._timerWorker) {
    Module._timerWorker.postMessage({
      type: 'stopMetro', index: index
    });
  }
});

/* ----------------------------------------------------------------
 * Metro state
 * ---------------------------------------------------------------- */

#define METRO_MIN_INTERVAL_MS 4.0

struct metro {
  bool running;
  int count;      /* ticks remaining, -1 = infinite */
  int stages;     /* total stages for finite counting */
  int tick_count; /* ticks fired so far */
  bool pending;   /* tick waiting to be processed */
};

static struct metro metros[METRO_COUNT];

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

void metro_init(void) {
  memset(metros, 0, sizeof(metros));
}

void metro_task(void) {
  for (int i = 0; i < METRO_COUNT; i++) {
    if (metros[i].running && metros[i].pending) {
      metros[i].pending = false;

      int current_count = metros[i].count;

      if (current_count > 0) {
        metros[i].count--;
        if (metros[i].count == 0) {
          metros[i].running = false;
          js_metro_stop(i);
        }
      }

      metros[i].tick_count++;

      /* match firmware: index is 0-based here, vm_handle_metro
       * adds 1 for Lua's 1-based indexing */
      vm_handle_metro(i, metros[i].tick_count);
    }
  }
}

void metro_set_with_count(int index, double s, int count) {
  if (index < 0 || index >= METRO_COUNT) return;

  struct metro *m = &metros[index];

  if (s > 0) {
    double interval_ms = s * 1000.0;

    if (interval_ms < METRO_MIN_INTERVAL_MS) {
      EM_ASM({
        console.warn('metro[' + $0 + ']: interval ' + $1 +
          'ms clamped to ' + $2 + 'ms (browser minimum)');
      }, index, interval_ms, METRO_MIN_INTERVAL_MS);
      interval_ms = METRO_MIN_INTERVAL_MS;
    }

    m->count = count;
    m->stages = count;
    m->tick_count = 0;
    m->pending = false;
    m->running = true;

    js_metro_start(index, interval_ms);
  } else {
    /* s <= 0: stop the metro */
    if (m->running) {
      js_metro_stop(index);
      m->running = false;
    }
  }
}

void metro_set(int index, double s) {
  metro_set_with_count(index, s, -1);
}

void metro_cleanup(void) {
  for (int i = 0; i < METRO_COUNT; i++) {
    if (metros[i].running) {
      js_metro_stop(i);
      metros[i].running = false;
    }
  }
}

/* ----------------------------------------------------------------
 * Called from JS when a setInterval timer fires
 * ---------------------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE
void viii_metro_tick(int index) {
  if (index >= 0 && index < METRO_COUNT && metros[index].running) {
    metros[index].pending = true;
  }
}
