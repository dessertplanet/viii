/*
 * fs_web.c — fs.h implementation for browser
 *
 * In-memory flat filesystem. Files are stored in a static table.
 * JS persists to IndexedDB asynchronously on writes.
 * On startup, JS preloads files via viii_fs_preload() before init.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>

#include "lfs.h"
#include "fs.h"

/* ----------------------------------------------------------------
 * JS bridge — async persistence to IndexedDB
 * ---------------------------------------------------------------- */

EM_JS(void, js_fs_persist, (const char *name, const void *data, uint32_t len), {
  if (Module.onFsPersist) {
    var fname = UTF8ToString(name);
    var bytes = new Uint8Array(HEAPU8.buffer, data, len).slice();
    Module.onFsPersist(fname, bytes);
  }
});

EM_JS(void, js_fs_remove, (const char *name), {
  if (Module.onFsRemove) {
    Module.onFsRemove(UTF8ToString(name));
  }
});

/* ----------------------------------------------------------------
 * In-memory file storage
 * ---------------------------------------------------------------- */

#define FS_MAX_FILES 64
#define FS_MAX_FILENAME 128
#define FS_TOTAL_SPACE (128 * 1024) /* 128KB virtual filesystem */

struct fs_entry {
  char name[FS_MAX_FILENAME];
  uint8_t *data;
  uint32_t size;
  uint32_t capacity;
  bool used;
};

static struct fs_entry entries[FS_MAX_FILES];

/* open file handles */
#define FS_MAX_OPEN 8

struct fs_handle {
  int entry;    /* index into entries[] */
  int32_t pos;  /* current read/write position */
  int flags;
  bool open;
};

static struct fs_handle handles[FS_MAX_OPEN];

/* ----------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------- */

static int find_entry(const char *name) {
  for (int i = 0; i < FS_MAX_FILES; i++) {
    if (entries[i].used && strcmp(entries[i].name, name) == 0)
      return i;
  }
  return -1;
}

static int alloc_entry(const char *name) {
  for (int i = 0; i < FS_MAX_FILES; i++) {
    if (!entries[i].used) {
      memset(&entries[i], 0, sizeof(struct fs_entry));
      strncpy(entries[i].name, name, FS_MAX_FILENAME - 1);
      entries[i].used = true;
      return i;
    }
  }
  return -1;
}

static int alloc_handle(void) {
  for (int i = 0; i < FS_MAX_OPEN; i++) {
    if (!handles[i].open) return i;
  }
  return -1;
}

static void ensure_capacity(struct fs_entry *e, uint32_t needed) {
  if (needed <= e->capacity) return;
  uint32_t newcap = needed < 256 ? 256 : needed * 2;
  e->data = realloc(e->data, newcap);
  e->capacity = newcap;
}

/* ----------------------------------------------------------------
 * Public API — init / mount
 * ---------------------------------------------------------------- */

void fs_init(void) {
  /* don't clear entries — they may be preloaded from IndexedDB */
  memset(handles, 0, sizeof(handles));
}

void fs_mount(void) {
  /* no-op; preloading happens from JS before viii_init() */
}

void fs_unmount(void) {
  /* no-op */
}

void fs_reformat(void) {
  for (int i = 0; i < FS_MAX_FILES; i++) {
    if (entries[i].used) {
      free(entries[i].data);
      entries[i].data = NULL;
      entries[i].used = false;
      js_fs_remove(entries[i].name);
    }
  }
}

/* ----------------------------------------------------------------
 * File list
 * ---------------------------------------------------------------- */

char **fs_get_file_list(uint32_t *num_files) {
  uint32_t count = 0;
  for (int i = 0; i < FS_MAX_FILES; i++) {
    if (entries[i].used) count++;
  }

  char **list = malloc(sizeof(char *) * (count + 1));
  uint32_t idx = 0;
  for (int i = 0; i < FS_MAX_FILES; i++) {
    if (entries[i].used) {
      list[idx] = strdup(entries[i].name);
      idx++;
    }
  }
  *num_files = count;
  return list;
}

uint32_t fs_get_free_space(void) {
  uint32_t used = 0;
  for (int i = 0; i < FS_MAX_FILES; i++) {
    if (entries[i].used) used += entries[i].size;
  }
  return (used < FS_TOTAL_SPACE) ? FS_TOTAL_SPACE - used : 0;
}

/* ----------------------------------------------------------------
 * File operations
 * ---------------------------------------------------------------- */

int fs_file_open(lfs_file_t *file, const char *path, int flags) {
  int h = alloc_handle();
  if (h < 0) return LFS_ERR_NOMEM;

  int e = find_entry(path);

  if (e < 0) {
    if (!(flags & LFS_O_CREAT)) return LFS_ERR_NOENT;
    e = alloc_entry(path);
    if (e < 0) return LFS_ERR_NOSPC;
  }

  /* truncate if writing to existing file with CREAT */
  if ((flags & LFS_O_CREAT) && (flags & LFS_O_WRONLY)) {
    entries[e].size = 0;
  }

  handles[h].entry = e;
  handles[h].pos = 0;
  handles[h].flags = flags;
  handles[h].open = true;

  file->_fd = h;
  return 0;
}

int fs_file_close(lfs_file_t *file) {
  int h = file->_fd;
  if (h < 0 || h >= FS_MAX_OPEN || !handles[h].open) return LFS_ERR_INVAL;

  /* persist to IndexedDB if file was written */
  if (handles[h].flags & (LFS_O_WRONLY | LFS_O_RDWR)) {
    int e = handles[h].entry;
    js_fs_persist(entries[e].name, entries[e].data, entries[e].size);
  }

  handles[h].open = false;
  file->_fd = -1;
  return 0;
}

int fs_file_sync(lfs_file_t *file) {
  (void)file;
  return 0;
}

int32_t fs_file_read(lfs_file_t *file, void *buffer, uint32_t size) {
  int h = file->_fd;
  if (h < 0 || h >= FS_MAX_OPEN || !handles[h].open) return LFS_ERR_INVAL;

  struct fs_entry *e = &entries[handles[h].entry];
  int32_t pos = handles[h].pos;
  int32_t avail = (int32_t)e->size - pos;
  if (avail <= 0) return 0;

  int32_t to_read = (int32_t)size < avail ? (int32_t)size : avail;
  memcpy(buffer, e->data + pos, to_read);
  handles[h].pos += to_read;
  return to_read;
}

int32_t fs_file_write(lfs_file_t *file, const void *buffer, uint32_t size) {
  int h = file->_fd;
  if (h < 0 || h >= FS_MAX_OPEN || !handles[h].open) return LFS_ERR_INVAL;

  struct fs_entry *e = &entries[handles[h].entry];
  int32_t pos = handles[h].pos;
  uint32_t end = (uint32_t)pos + size;

  ensure_capacity(e, end);
  memcpy(e->data + pos, buffer, size);
  handles[h].pos = (int32_t)end;
  if (end > e->size) e->size = end;

  return (int32_t)size;
}

int32_t fs_file_seek(lfs_file_t *file, int32_t off, int whence) {
  int h = file->_fd;
  if (h < 0 || h >= FS_MAX_OPEN || !handles[h].open) return LFS_ERR_INVAL;

  struct fs_entry *e = &entries[handles[h].entry];
  int32_t newpos;

  switch (whence) {
    case LFS_SEEK_SET: newpos = off; break;
    case LFS_SEEK_CUR: newpos = handles[h].pos + off; break;
    case LFS_SEEK_END: newpos = (int32_t)e->size + off; break;
    default: return LFS_ERR_INVAL;
  }

  if (newpos < 0) newpos = 0;
  handles[h].pos = newpos;
  return newpos;
}

int fs_file_truncate(lfs_file_t *file, uint32_t size) {
  int h = file->_fd;
  if (h < 0 || h >= FS_MAX_OPEN || !handles[h].open) return LFS_ERR_INVAL;
  entries[handles[h].entry].size = size;
  return 0;
}

int32_t fs_file_tell(lfs_file_t *file) {
  int h = file->_fd;
  if (h < 0 || h >= FS_MAX_OPEN || !handles[h].open) return LFS_ERR_INVAL;
  return handles[h].pos;
}

int fs_file_rewind(lfs_file_t *file) {
  int h = file->_fd;
  if (h < 0 || h >= FS_MAX_OPEN || !handles[h].open) return LFS_ERR_INVAL;
  handles[h].pos = 0;
  return 0;
}

int32_t fs_file_size(lfs_file_t *file) {
  int h = file->_fd;
  if (h < 0 || h >= FS_MAX_OPEN || !handles[h].open) return 0;
  return (int32_t)entries[handles[h].entry].size;
}

int fs_remove(const char *path) {
  int e = find_entry(path);
  if (e < 0) return LFS_ERR_NOENT;
  free(entries[e].data);
  entries[e].data = NULL;
  entries[e].used = false;
  js_fs_remove(path);
  return 0;
}

int fs_rename(const char *oldpath, const char *newpath) {
  int e = find_entry(oldpath);
  if (e < 0) return LFS_ERR_NOENT;
  js_fs_remove(oldpath);
  strncpy(entries[e].name, newpath, FS_MAX_FILENAME - 1);
  js_fs_persist(entries[e].name, entries[e].data, entries[e].size);
  return 0;
}

int fs_stat(const char *path, struct lfs_info *info) {
  int e = find_entry(path);
  if (e < 0) return LFS_ERR_NOENT;
  info->size = entries[e].size;
  info->type = LFS_TYPE_REG;
  strncpy(info->name, entries[e].name, sizeof(info->name) - 1);
  return LFS_ERR_OK;
}

int32_t fs_fs_size(void) {
  return (int32_t)FS_TOTAL_SPACE;
}

/* ----------------------------------------------------------------
 * Called from JS at startup to preload files from IndexedDB
 * ---------------------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE
void viii_fs_preload(const char *name, const uint8_t *data, uint32_t len) {
  int e = find_entry(name);
  if (e < 0) {
    e = alloc_entry(name);
    if (e < 0) return;
  }
  ensure_capacity(&entries[e], len);
  memcpy(entries[e].data, data, len);
  entries[e].size = len;
}

/* Clear in-memory FS without touching IndexedDB (for device swap) */
EMSCRIPTEN_KEEPALIVE
void viii_fs_clear(void) {
  for (int i = 0; i < FS_MAX_FILES; i++) {
    if (entries[i].used) {
      free(entries[i].data);
      entries[i].data = NULL;
      entries[i].used = false;
    }
  }
  memset(handles, 0, sizeof(handles));
}
