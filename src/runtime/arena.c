#include "arena.h"

#include <stdint.h>
#include <stdlib.h>

#if defined(_MSC_VER)
#define CRUFT_TLS __declspec(thread)
#else
#define CRUFT_TLS _Thread_local
#endif

enum { CRUFT_ARENA_CHUNK = 1 << 20 };

typedef struct Chunk {
  struct Chunk *prev;
  size_t capacity;
  size_t used;
  unsigned char *base;
} Chunk;

typedef struct Mark {
  Chunk *chunk;
  size_t used;
} Mark;

typedef struct State {
  Chunk *top;
  Mark *marks;
  size_t mark_len;
  size_t mark_cap;
} State;

static CRUFT_TLS State g_arena;

static size_t align_up(size_t value, size_t align) {
  size_t rem;
  if (align <= 1)
    return value;
  if ((align & (align - 1)) == 0)
    return (value + align - 1) & ~(align - 1);
  rem = value % align;
  return rem == 0 ? value : value + (align - rem);
}

static Chunk *chunk_new(size_t capacity) {
  size_t bytes = sizeof(Chunk) + capacity + 64;
  unsigned char *raw = (unsigned char *)malloc(bytes);
  Chunk *chunk;
  uintptr_t start;
  uintptr_t aligned;
  if (!raw)
    abort();
  chunk = (Chunk *)raw;
  chunk->prev = NULL;
  chunk->capacity = capacity;
  chunk->used = 0;
  start = (uintptr_t)(chunk + 1);
  aligned = (start + 63u) & ~(uintptr_t)63u;
  chunk->base = (unsigned char *)aligned;
  return chunk;
}

void cruft_arena_push(void) {
  Mark *grown;
  if (g_arena.mark_len == g_arena.mark_cap) {
    g_arena.mark_cap = g_arena.mark_cap == 0 ? 8 : g_arena.mark_cap * 2;
    grown = (Mark *)realloc(g_arena.marks, g_arena.mark_cap * sizeof(Mark));
    if (!grown)
      abort();
    g_arena.marks = grown;
  }
  g_arena.marks[g_arena.mark_len].chunk = g_arena.top;
  g_arena.marks[g_arena.mark_len].used = g_arena.top ? g_arena.top->used : 0;
  g_arena.mark_len++;
}

void cruft_arena_pop(void) {
  Mark mark;
  if (g_arena.mark_len == 0)
    abort();
  g_arena.mark_len--;
  mark = g_arena.marks[g_arena.mark_len];
  while (g_arena.top != mark.chunk) {
    Chunk *dead = g_arena.top;
    if (!dead)
      abort();
    g_arena.top = dead->prev;
    free(dead);
  }
  if (g_arena.top)
    g_arena.top->used = mark.used;
}

void *cruft_arena_alloc(size_t nbytes, size_t align) {
  size_t off;
  size_t need;
  if (g_arena.mark_len == 0)
    abort();
  if (align == 0)
    align = 1;
  if (nbytes == 0)
    nbytes = 1;
  need = nbytes + align;
  if (!g_arena.top || align_up(g_arena.top->used, align) + nbytes > g_arena.top->capacity) {
    size_t cap = CRUFT_ARENA_CHUNK;
    Chunk *chunk;
    if (need > cap)
      cap = need;
    chunk = chunk_new(cap);
    chunk->prev = g_arena.top;
    g_arena.top = chunk;
  }
  off = align_up(g_arena.top->used, align);
  g_arena.top->used = off + nbytes;
  return g_arena.top->base + off;
}
