/* tusb.h shim — provides stubs for TinyUSB functions referenced
 * by iii framework code that we compile (metro.c references tud_cdc).
 * The actual I/O is handled by our web platform implementations. */
#ifndef TUSB_H_SHIM
#define TUSB_H_SHIM

#include <stdint.h>
#include <stdbool.h>

/* CDC stubs */
static inline void tud_init(uint8_t rhport) { (void)rhport; }
static inline void tud_task(void) {}
static inline uint32_t tud_cdc_n_available(uint8_t itf) { (void)itf; return 0; }
static inline uint32_t tud_cdc_n_read(uint8_t itf, void *buf, uint32_t sz) {
  (void)itf; (void)buf; (void)sz; return 0;
}
static inline uint32_t tud_cdc_n_write(uint8_t itf, const void *buf, uint32_t sz) {
  (void)itf; (void)buf; (void)sz; return sz;
}
static inline uint32_t tud_cdc_n_write_available(uint8_t itf) { (void)itf; return 64; }
static inline uint32_t tud_cdc_n_write_char(uint8_t itf, char c) { (void)itf; (void)c; return 1; }
static inline uint32_t tud_cdc_n_write_flush(uint8_t itf) { (void)itf; return 0; }

/* MIDI stubs */
static inline bool tud_midi_available(void) { return false; }
static inline uint32_t tud_midi_packet_read(uint8_t *buf) { (void)buf; return 0; }
static inline uint32_t tud_midi_stream_write(uint8_t cable, const uint8_t *buf, uint32_t sz) {
  (void)cable; (void)buf; (void)sz; return sz;
}

#endif
