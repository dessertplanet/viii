/* hardware/watchdog.h shim */
#ifndef HARDWARE_WATCHDOG_H_SHIM
#define HARDWARE_WATCHDOG_H_SHIM

#include <stdint.h>

/* In the browser, "reboot" triggers a VM reinit via JS */
extern void viii_request_reinit(void);

static inline void watchdog_reboot(uint32_t a, uint32_t b, uint32_t c) {
  (void)a; (void)b; (void)c;
  viii_request_reinit();
}

#endif
