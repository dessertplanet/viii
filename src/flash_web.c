/*
 * flash_web.c — flash.h stubs for browser
 *
 * Mode is always 0 (iii mode). Flash storage is not applicable.
 */

#include <stdint.h>
#include "flash.h"

void flash_init(void) {}
void flash_write_mode(uint8_t m) { (void)m; }
uint8_t flash_read_mode(void) { return 0; }
