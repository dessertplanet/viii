/*
 * js_bridge.js — Emscripten JS library
 *
 * Declares the JS functions that C code calls via EM_JS.
 * This file is passed to emcc via --js-library.
 * The actual implementations are in the EM_JS blocks within
 * the C source files; this file exists to satisfy the linker
 * for any additional bridge functions if needed.
 */

mergeInto(LibraryManager.library, {
  /* placeholder — all current bridges use EM_JS inline */
});
