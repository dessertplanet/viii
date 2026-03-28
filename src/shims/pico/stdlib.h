/* pico/stdlib.h shim for Emscripten builds */
#ifndef PICO_STDLIB_H_SHIM
#define PICO_STDLIB_H_SHIM

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static inline void sleep_ms(uint32_t ms) { (void)ms; }
static inline void sleep_us(uint64_t us) { (void)us; }

#endif
