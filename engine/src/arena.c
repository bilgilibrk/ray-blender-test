#include "engine/arena.h"

#include <stdlib.h>
#include <string.h>

#define ARENA_ALIGN 16

static size_t AlignUp(size_t n, size_t align) { return (n + align - 1) & ~(align - 1); }

bool ArenaInit(Arena *a, size_t capacity, const char *name)
{
    a->base = (unsigned char *)calloc(1, capacity);
    a->capacity = a->base ? capacity : 0;
    a->used = 0;
    a->peak = 0;
    a->name = name;
    return a->base != NULL;
}

void ArenaFree(Arena *a)
{
    free(a->base);
    a->base = NULL;
    a->capacity = a->used = a->peak = 0;
}

void *ArenaAlloc(Arena *a, size_t size)
{
    if (size == 0) return NULL;
    size_t offset = AlignUp(a->used, ARENA_ALIGN);
    if (offset + size > a->capacity) return NULL;
    void *p = a->base + offset;
    a->used = offset + size;
    if (a->used > a->peak) a->peak = a->used;
    return p;
}

char *ArenaStrDup(Arena *a, const char *str, size_t len)
{
    char *out = (char *)ArenaAlloc(a, len + 1);
    if (!out) return NULL;
    memcpy(out, str, len);
    out[len] = '\0';
    return out;
}

void ArenaReset(Arena *a)
{
    memset(a->base, 0, a->used);
    a->used = 0;
}
