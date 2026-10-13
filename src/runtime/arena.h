/* Runtime bump allocator used by `scope arena`.
 *
 * cruft_arena_push saves the watermark. cruft_arena_pop restores it and frees
 * any chunks created inside the scope. Individual objects are not freed.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void cruft_arena_push(void);
void cruft_arena_pop(void);
void *cruft_arena_alloc(size_t nbytes, size_t align);

#ifdef __cplusplus
}
#endif
