/* Decimal and boolean formatting for std/fmt.cruft.
 *
 * The caller owns `buf` and passes its capacity, including the trailing NUL.
 * The returned length does not count that NUL. A short buffer returns -1.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>

static int64_t finish(char *buf, int64_t cap, int written) {
  if (!buf || cap <= 1 || written < 0 || (int64_t)written >= cap)
    return -1;
  return (int64_t)written;
}

int64_t cruft_fmt_i64(char *buf, int64_t cap, int64_t n) {
  return finish(buf, cap, snprintf(buf, (size_t)cap, "%" PRId64, n));
}

int64_t cruft_fmt_u64(char *buf, int64_t cap, uint64_t n) {
  return finish(buf, cap, snprintf(buf, (size_t)cap, "%" PRIu64, n));
}

int64_t cruft_fmt_f64(char *buf, int64_t cap, double n) {
  return finish(buf, cap, snprintf(buf, (size_t)cap, "%g", n));
}
