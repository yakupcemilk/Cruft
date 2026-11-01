/* pthread spawn and join. The handle is a heap pthread_t*. */
#ifndef CRUFT_THREAD_H
#define CRUFT_THREAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*cruft_thread_fn)(int64_t *);

void *cruft_thread_spawn(cruft_thread_fn fn, int64_t *state);
int cruft_thread_join(void *handle);

#ifdef __cplusplus
}
#endif

#endif /* CRUFT_THREAD_H */
