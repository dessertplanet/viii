/* pico/unique_id.h shim */
#ifndef PICO_UNIQUE_ID_H_SHIM
#define PICO_UNIQUE_ID_H_SHIM
#include <stdint.h>
typedef struct { uint8_t id[8]; } pico_unique_board_id_t;
static inline void pico_get_unique_board_id(pico_unique_board_id_t *t) {
  for (int i = 0; i < 8; i++) t->id[i] = (uint8_t)i;
}
#endif
