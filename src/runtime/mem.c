#include "mem.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int64_t cruft_alloc(int64_t nbytes) {
  void *block;
  if (nbytes <= 0)
    return 0;
  block = malloc((size_t)nbytes);
  if (!block)
    return 0;
  return (int64_t)(uintptr_t)block;
}

void cruft_free(int64_t addr) {
  if (addr == 0)
    return;
  free((void *)(uintptr_t)addr);
}

int64_t cruft_realloc(int64_t addr, int64_t nbytes) {
  void *block;
  if (nbytes <= 0) {
    cruft_free(addr);
    return 0;
  }
  block = realloc(addr == 0 ? NULL : (void *)(uintptr_t)addr, (size_t)nbytes);
  if (!block)
    return 0;
  return (int64_t)(uintptr_t)block;
}

void cruft_bounds_fail(void) {
  fputs("cruft: index out of bounds\n", stderr);
  abort();
}
