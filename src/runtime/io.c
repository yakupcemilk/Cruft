#include "io.h"

#include <stdio.h>
#include <stdint.h>

void *cruft_stdout(void) { return stdout; }

void *cruft_stderr(void) { return stderr; }

void *cruft_stdin(void) { return stdin; }

void cruft_io_write(const void *data, int64_t len, void *stream) {
  if (!data || !stream || len <= 0)
    return;
  fwrite(data, 1, (size_t)len, (FILE *)stream);
}

int64_t cruft_read_line(char *buf, int64_t cap) {
  int ncap;
  int64_t n;
  if (!buf || cap < 2)
    return -1;
  ncap = cap > 2147483647 ? 2147483647 : (int)cap;
  if (!fgets(buf, ncap, stdin))
    return -1;
  n = 0;
  while (buf[n] != '\0')
    n++;
  if (n > 0 && buf[n - 1] == '\n') {
    buf[--n] = '\0';
    if (n > 0 && buf[n - 1] == '\r')
      buf[--n] = '\0';
  }
  return n;
}
