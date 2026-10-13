/* Stable file ABI for std/fs.cruft.
 *
 * Pointers cross as machine addresses. Offsets and transfer sizes are int64_t
 * on both LP64 and LLP64, so std/fs.cruft does not have to know that `long`
 * is 32 bits on Windows. The bodies call fopen, fread, fwrite, fseek, ftell
 * and fclose.
 */
#ifndef CRUFT_FS_H
#define CRUFT_FS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *cruft_fopen(const char *path, const char *mode);
int64_t cruft_fread(void *buf, int64_t size, int64_t count, void *stream);
int64_t cruft_fwrite(const void *buf, int64_t size, int64_t count, void *stream);
int32_t cruft_fseek(void *stream, int64_t offset, int32_t whence);
int64_t cruft_ftell(void *stream);
int32_t cruft_fclose(void *stream);
int32_t cruft_exists(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CRUFT_FS_H */
