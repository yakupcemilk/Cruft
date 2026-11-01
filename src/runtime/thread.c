#include "thread.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

struct cruft_thread_start {
  cruft_thread_fn fn;
  int64_t *state;
};

static void *cruft_thread_main(void *arg) {
  struct cruft_thread_start *start = arg;
  cruft_thread_fn fn = start->fn;
  int64_t *state = start->state;
  free(start);
  int rc = fn(state);
  return (void *)(intptr_t)rc;
}

void *cruft_thread_spawn(cruft_thread_fn fn, int64_t *state) {
  struct cruft_thread_start *start = malloc(sizeof *start);
  pthread_t *th;
  if (!start)
    return 0;
  start->fn = fn;
  start->state = state;
  th = malloc(sizeof *th);
  if (!th) {
    free(start);
    return 0;
  }
  if (pthread_create(th, 0, cruft_thread_main, start) != 0) {
    free(th);
    free(start);
    return 0;
  }
  return th;
}

int cruft_thread_join(void *handle) {
  pthread_t *th = handle;
  void *ret = 0;
  if (!th)
    return -1;
  if (pthread_join(*th, &ret) != 0) {
    free(th);
    return -1;
  }
  free(th);
  return (int)(intptr_t)ret;
}
