#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#endif

#include "fs.h"

#include <stdio.h>
#include <stdint.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

void *cruft_fopen(const char *path, const char *mode) {
  if (!path || !mode)
    return NULL;
  return fopen(path, mode);
}

int64_t cruft_fread(void *buf, int64_t size, int64_t count, void *stream) {
  if (!buf || !stream || size <= 0 || count < 0)
    return 0;
  return (int64_t)fread(buf, (size_t)size, (size_t)count, (FILE *)stream);
}

int64_t cruft_fwrite(const void *buf, int64_t size, int64_t count, void *stream) {
  if (!buf || !stream || size <= 0 || count < 0)
    return 0;
  return (int64_t)fwrite(buf, (size_t)size, (size_t)count, (FILE *)stream);
}

int32_t cruft_fseek(void *stream, int64_t offset, int32_t whence) {
  if (!stream)
    return -1;
#if defined(_WIN32)
  return (int32_t)_fseeki64((FILE *)stream, (__int64)offset, (int)whence);
#else
  return (int32_t)fseeko((FILE *)stream, (off_t)offset, (int)whence);
#endif
}

int64_t cruft_ftell(void *stream) {
  if (!stream)
    return -1;
#if defined(_WIN32)
  return (int64_t)_ftelli64((FILE *)stream);
#else
  return (int64_t)ftello((FILE *)stream);
#endif
}

int32_t cruft_fclose(void *stream) {
  if (!stream)
    return -1;
  return (int32_t)fclose((FILE *)stream);
}

int32_t cruft_exists(const char *path) {
  if (!path)
    return 0;
#if defined(_WIN32)
  return _access(path, 0) == 0 ? 1 : 0;
#else
  return access(path, F_OK) == 0 ? 1 : 0;
#endif
}
