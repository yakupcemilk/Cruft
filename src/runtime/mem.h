/* Heap wrappers with an integer address ABI.
 *
 * std/mem.cruft exposes these as i64. The compiler calls the same symbols for
 * Vector and String storage and bit-casts the address to `ptr`.
 */
#ifndef CRUFT_MEM_H
#define CRUFT_MEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t cruft_alloc(int64_t nbytes);
void cruft_free(int64_t addr);
int64_t cruft_realloc(int64_t addr, int64_t nbytes);
/* Index outside [0, len). Prints to stderr and aborts. */
void cruft_bounds_fail(void);

#ifdef __cplusplus
}
#endif

#endif /* CRUFT_MEM_H */
