/* pico/bootrom.h shim — rom_reset_usb_boot is a no-op in browser */
#ifndef PICO_BOOTROM_H_SHIM
#define PICO_BOOTROM_H_SHIM

#include <stdint.h>

static inline void rom_reset_usb_boot(uint32_t a, uint32_t b) {
  (void)a; (void)b;
  /* bootloader mode not applicable in browser */
}

#endif
