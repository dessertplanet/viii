/*
 * flash_web.c — flash.h stubs for browser
 *
 * Mode is always 0 (iii mode). Flash storage is not applicable.
 */

#include <stdint.h>
#include "flash.h"

void flash_init(void) {}

void flash_write_mode(uint8_t m) { (void)m; }

uint8_t flash_read_mode(void) { return 0; /* always iii mode */ }

uint32_t flash_get_status_offset(void) { return 0; }
uint32_t flash_get_fs_start(void) { return 0; }
uint32_t flash_get_fs_offset(void) { return 0; }
uint32_t flash_get_fs_num_blocks(void) { return 256; }
