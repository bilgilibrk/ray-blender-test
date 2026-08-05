// Bump allocator. Level data is allocated in one arena and freed in one call,
// which keeps the loader free of per-node ownership bookkeeping.
#ifndef ENGINE_ARENA_H
#define ENGINE_ARENA_H

#include <stdbool.h>
#include <stddef.h>

typedef struct Arena {
    unsigned char *base;
    size_t capacity;
    size_t used;
    size_t peak;
    const char *name;
} Arena;

// Reserves `capacity` bytes up front; returns false if the allocation fails.
bool ArenaInit(Arena *a, size_t capacity, const char *name);
void ArenaFree(Arena *a);

// Returns zeroed, pointer-aligned memory, or NULL when the arena is exhausted.
void *ArenaAlloc(Arena *a, size_t size);
char *ArenaStrDup(Arena *a, const char *str, size_t len);

// Drops every allocation without releasing the backing memory.
void ArenaReset(Arena *a);

#endif // ENGINE_ARENA_H
