/* pico/critical_section.h shim — no-ops (single-threaded in browser) */
#ifndef PICO_CRITICAL_SECTION_H_SHIM
#define PICO_CRITICAL_SECTION_H_SHIM

typedef struct { int _dummy; } critical_section_t;

static inline void critical_section_init(critical_section_t *cs) { (void)cs; }
static inline void critical_section_enter_blocking(critical_section_t *cs) { (void)cs; }
static inline void critical_section_exit(critical_section_t *cs) { (void)cs; }

#endif
