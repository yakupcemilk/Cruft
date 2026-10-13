/* Stable stdio ABI for std/io.cruft.
 *
 * stdout, stderr and stdin are returned as pointers so Cruft never names the
 * FILE objects itself. cruft_io_write is fwrite on one of those streams.
 * cruft_read_line is fgets, with the trailing newline removed.
 */
#ifndef CRUFT_IO_H
#define CRUFT_IO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *cruft_stdout(void);
void *cruft_stderr(void);
void *cruft_stdin(void);
void cruft_io_write(const void *data, int64_t len, void *stream);
int64_t cruft_read_line(char *buf, int64_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CRUFT_IO_H */
