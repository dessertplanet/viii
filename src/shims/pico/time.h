/* pico/time.h shim — provides get_absolute_time / to_us_since_boot
 * using emscripten_get_now() (millisecond wallclock). */
#ifndef PICO_TIME_H_SHIM
#define PICO_TIME_H_SHIM

#include <stdint.h>
#include <emscripten.h>

typedef uint64_t absolute_time_t;

static inline absolute_time_t get_absolute_time(void) {
  /* emscripten_get_now() returns ms as double; convert to µs */
  return (absolute_time_t)(emscripten_get_now() * 1000.0);
}

static inline uint64_t to_us_since_boot(absolute_time_t t) {
  return (uint64_t)t;
}

#endif
