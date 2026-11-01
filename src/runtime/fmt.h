#ifndef CRUFT_FMT_H
#define CRUFT_FMT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t cruft_fmt_i64(char *buf, int64_t cap, int64_t n);
int64_t cruft_fmt_u64(char *buf, int64_t cap, uint64_t n);
int64_t cruft_fmt_f64(char *buf, int64_t cap, double n);

#ifdef __cplusplus
}
#endif

#endif /* CRUFT_FMT_H */
