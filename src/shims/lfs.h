/*
 * lfs.h shim — minimal LittleFS type definitions so iii framework
 * code (vm.c, repl.c) compiles. The actual filesystem implementation
 * is in fs_web.c using an in-memory store.
 */
#ifndef LFS_H_SHIM
#define LFS_H_SHIM

#include <stdint.h>

/* file descriptor — index into fs_web's open file table */
typedef struct {
  int _fd;
} lfs_file_t;

typedef struct {
  int _dummy;
} lfs_dir_t;

struct lfs_info {
  uint32_t size;
  uint8_t type;
  char name[256];
};

struct lfs_fsinfo {
  uint32_t block_size;
  uint32_t block_count;
};

/* flags (matching standard littlefs values) */
#define LFS_O_RDONLY 1
#define LFS_O_WRONLY 2
#define LFS_O_RDWR   3
#define LFS_O_CREAT  0x0100
#define LFS_O_EXCL   0x0200
#define LFS_O_TRUNC  0x0400
#define LFS_O_APPEND 0x0800

/* error codes */
#define LFS_ERR_OK          0
#define LFS_ERR_IO         -5
#define LFS_ERR_CORRUPT   -84
#define LFS_ERR_NOENT      -2
#define LFS_ERR_EXIST     -17
#define LFS_ERR_NOTDIR    -20
#define LFS_ERR_ISDIR     -21
#define LFS_ERR_NOTEMPTY  -39
#define LFS_ERR_INVAL     -22
#define LFS_ERR_NOSPC     -28
#define LFS_ERR_NOMEM     -12
#define LFS_ERR_NOATTR   -61
#define LFS_ERR_NAMETOOLONG -36

/* type enum */
enum lfs_type {
  LFS_TYPE_REG = 0x001,
  LFS_TYPE_DIR = 0x002,
};

/* seek whence */
#define LFS_SEEK_SET 0
#define LFS_SEEK_CUR 1
#define LFS_SEEK_END 2

#endif
