# 02 — Memory: arena allocation

> `engine/include/engine/arena.h` · `engine/src/arena.c` — 80 lines total.

---

## The problem

Loading a level allocates a lot of small, related things: an array of props, a
string for each prop's model name, arrays of colliders, sand traps, spawns,
waypoints, checkpoints and lights. `levels/circuit02.level.json` produces 1,237
props, 300 colliders, 78 sand traps, 489 waypoints and 32 lights — plus 1,237
interned strings.

With `malloc`, that is about 1,250 separate allocations — one per array plus one
per interned model name — all of which must be freed exactly once, in the right
order, including on every early-return path in the loader. Get one wrong and you
have a leak; free one twice and you have a crash that reproduces on Tuesdays.

The traditional answers are reference counting (slow, viral), a garbage
collector (not in C), or extreme discipline (works until someone adds an
`if (!model) continue;` in the middle of the loop — which `level.c` does have).

## The idea

Notice what all of those allocations have in common: **they have exactly the
same lifetime.** They are born when the level loads and they die when it unloads. Not
one of them individually outlives the level.

When lifetimes coincide, individual ownership is bookkeeping you are paying for
and not using. So:

> Allocate one block up front. Hand out slices of it by bumping a pointer. Free
> the whole block at once.

That is an **arena** (also called a bump, linear, or region allocator).

---

## The implementation

The entire data structure:

```c
// engine/include/engine/arena.h
typedef struct Arena {
    unsigned char *base;
    size_t capacity;
    size_t used;
    size_t peak;
    const char *name;
} Arena;
```

`base` and `capacity` are the block. `used` is the bump pointer, as an offset.
`peak` and `name` are for diagnostics only.

### Allocation

```c
// engine/src/arena.c
#define ARENA_ALIGN 16

static size_t AlignUp(size_t n, size_t align) { return (n + align - 1) & ~(align - 1); }

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
```

Six lines of logic. Some things worth pulling apart:

**`AlignUp` and the bit trick.** `(n + align - 1) & ~(align - 1)` rounds `n` up
to a multiple of `align`, and only works because `align` is a power of two. With
`align = 16`, `~(align - 1)` is `~0b1111`, i.e. a mask that clears the low four
bits. Adding 15 first pushes anything not already aligned up into the next
multiple, and the mask then truncates back down to it. Try it: `n = 17` gives
`(17 + 15) & ~15 = 32 & ~15 = 32`. And `n = 32` gives `47 & ~15 = 32` — already
aligned values are unchanged, which is what you need.

**Why 16 bytes?** It is the strictest alignment any type in this project needs.
`Vector3` and `Matrix` are floats; SSE loads of a `Matrix` want 16. Over-aligning
is safe and wastes at most 15 bytes per allocation; under-aligning is undefined
behaviour and, on ARM, a real crash. C11's `max_align_t` would be the portable
answer; 16 is the pragmatic one.

**Failure is a `NULL` return, not a crash.** This matters more than it looks —
see the testing section below.

**No `free` for individual allocations.** There is no `ArenaFreeOne`. That is
the entire point. You cannot return a slice to the middle of a bump allocator
without either a free list (at which point you have written `malloc`) or
compaction (at which point you need to fix up every pointer).

### Initialisation and teardown

```c
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
```

`calloc` rather than `malloc` is deliberate: **every allocation comes back
zeroed.** The level loader relies on this — it can allocate a `LevelProp` array
and fill only the fields present in the JSON, knowing the rest are zero rather
than garbage. That removes a whole category of "forgot to initialise a field"
bug, and one that is especially nasty because it usually looks fine in a debug
build where the OS happens to hand you zeroed pages anyway.

Note the failure path sets `capacity = 0`. That way a failed arena is not merely
"returned false" — every subsequent `ArenaAlloc` on it also fails cleanly,
because `offset + size > 0` is true for any positive size. A caller who ignores
the return value gets `NULL` pointers rather than writes into `NULL + offset`.

### Reset

```c
void ArenaReset(Arena *a)
{
    memset(a->base, 0, a->used);
    a->used = 0;
}
```

Drops every allocation without returning memory to the OS, so the arena can be
refilled without a fresh `calloc`. It re-zeros `used` bytes to preserve the
"allocations come back zeroed" guarantee.

### String interning

```c
char *ArenaStrDup(Arena *a, const char *str, size_t len)
{
    char *out = (char *)ArenaAlloc(a, len + 1);
    if (!out) return NULL;
    memcpy(out, str, len);
    out[len] = '\0';
    return out;
}
```

Used by the level loader for prop model names. The name in the parsed JSON tree
lives in the *scratch* arena, which is about to be thrown away, so it has to be
copied into the *level* arena that survives.

---

## Arenas in use

There are five in the codebase, each with a clearly-stated lifetime:

| Arena | Owner | Holds | Lives until |
|---|---|---|---|
| `"level-scratch"` | `LevelLoad` (local) | the parsed JSON tree | end of `LevelLoad` |
| `"level"` | `Level` | props, colliders, traps, spawns, waypoints, gates, lights, model-name strings | `LevelUnload` |
| `"spline"` | `Spline` | the resampled centre-line samples | `SplineFree` |
| `"collision"` | `CollisionWorld` | boxes, heights, grid arrays | `CollisionWorldFree` |
| `"race"` | `Race` | per-gate arc lengths | `RaceFree` |

### The two-arena trick in the level loader

This is the pattern worth stealing:

```c
// engine/src/level.c
// The JSON tree is scratch: it is parsed, copied out, and thrown away, so it
// lives in its own arena rather than bloating the level for its whole life.
Arena scratch = { 0 };
size_t scratchBytes = (size_t)fileSize * 12 + (1u << 20);
if (!ArenaInit(&scratch, scratchBytes, "level-scratch")) { /* ... */ }

JsonValue *root = JsonParse(&scratch, (const char *)text, (size_t)fileSize, err, sizeof err);
/* ... read everything out into level->arena ... */
ArenaFree(&scratch);
```

Two arenas with different lifetimes. The parsed tree is enormously bigger than
the data you actually want — every number becomes a 24-byte `JsonValue`, every
array element gets a linked-list node during parsing — but none of it survives
the function. `circuit02.level.json` is 342 KB on disk and its parse tree runs
to megabytes; the resulting `Level` arena is 97 KB.

**The sizing heuristic.** `fileSize * 12 + 1 MB` is not derived; it is measured
and padded. The 12× covers the worst realistic expansion ratio (short numeric
tokens becoming `JsonValue` structs plus `ItemNode` links), and the flat
megabyte covers small files where a multiplier alone would be too tight. If it
is ever wrong, `JsonParse` returns `NULL` with `"out of arena memory"` and the
level fails to load with a clear message — which is a far better failure than
silent truncation.

### Sizing the level arena

The loader does not guess. It walks the parsed tree and computes the exact
requirement before allocating:

```c
// engine/src/level.c
static size_t EstimateLevelBytes(const JsonValue *root, int autoCheckpoints)
{
    size_t bytes = 0;
    bytes += sizeof(LevelProp) * (size_t)JsonCount(props);
    bytes += sizeof(LevelCollider) * (size_t)JsonCount(colliders);
    /* ... one line per array ... */
    for (int i = 0; i < JsonCount(props); i++) {
        bytes += strlen(JsonStringField(JsonAt(props, i), "model", "")) + 1;
    }
    // 16-byte alignment padding per array, plus one per interned string.
    bytes += 16 * (size_t)(8 + JsonCount(props));
    return bytes + 1024;
}
```

This is only possible because the JSON has already been parsed — the counts are
known. It is the standard **two-pass allocation** shape: measure, allocate once,
fill. You will see it again in `SplineBuild` (count subdivisions, then allocate),
in `CollisionWorldBuild` (count per-cell references, then allocate), and in
`StaticBatchBuild` (count vertices per chunk, then allocate).

The `16 * (8 + propCount)` term is the alignment slack: each of the ~8 arrays
may waste up to 15 bytes rounding up, and so may each interned string.

---

## What this buys, concretely

Compare the two versions of `LevelUnload`:

```c
// With arenas — the real code.
void LevelUnload(Level *level)
{
    ArenaFree(&level->arena);
    memset(level, 0, sizeof(*level));
}
```

```c
// Without arenas — what you would otherwise write.
void LevelUnload(Level *level)
{
    for (int i = 0; i < level->propCount; i++) free((void *)level->props[i].model);
    free(level->props);
    free(level->colliders);
    free(level->sandtraps);
    free(level->spawns);
    free(level->waypoints);
    free(level->checkpoints);
    free(level->lights);
    memset(level, 0, sizeof(*level));
}
```

The second is not *hard*, it is just something you have to keep right forever.
Add a field, remember the free. Add an early return in the loader, remember to
unwind whatever you allocated so far. The arena version has no such maintenance
surface: adding a field to `Level` changes `EstimateLevelBytes` and nothing else.

There are performance benefits too — one `calloc` instead of thousands of
`malloc`s, and the data ends up contiguous, so iterating props is cache-friendly
— but they are secondary. **The real win is that lifetime bugs stop being
possible.**

---

## Where the arena is the wrong answer

Three of the five subsystems that could use an arena deliberately do not.

**The static batch** (`render.c`) uses `MemAlloc`/`calloc` for its chunk vertex
arrays. Why? Because `UploadMesh` hands those pointers to raylib, which then
owns them and frees them in `UnloadMesh`. You cannot hand raylib a pointer into
the middle of an arena and let it call `free` on it.

**The terrain** (`terrain.c`) uses `calloc` for the same reason, plus its
heightfield is resized by a doubling loop before the final size is known.

**The skid trail ring buffer** (`skid.c`) is a fixed-size array inside the
struct. It never allocates at all:

```c
// game/include/game/skid.h
typedef struct SkidTrails {
    SkidQuad quads[SKID_MAX_QUADS];
    SkidEmitter emitters[SKID_EMITTERS];
    int next;
} SkidTrails;
```

That is the best allocator of all: none. When the maximum count is small and
known, a fixed array beats every dynamic scheme, and the ring buffer's overwrite
semantics are exactly the "oldest marks fade first" behaviour you wanted anyway.

The lesson is not "arenas everywhere". It is: **match the allocator to the
lifetime pattern.** Coincident lifetimes → arena. Foreign ownership → whatever
the foreign code expects. Bounded count → a fixed array.

---

## Allocation failure as a first-class case

This is the subtlest benefit and the one most often missed.

Because `ArenaAlloc` returns `NULL` on exhaustion rather than aborting, and
because the arena's capacity is a parameter, **you can test the out-of-memory
path deterministically**. `tests/test_json.c` does:

```c
// tests/test_json.c
// Exhausting the arena must fail cleanly.
Arena tiny;
ArenaInit(&tiny, 64, "tiny");
CHECK(P(&tiny, doc, err, sizeof err) == NULL, "arena exhaustion handled");
ArenaFree(&tiny);
```

Try writing that test against `malloc`. You would need to interpose the
allocator, or run under a fault-injecting harness, or set `RLIMIT_AS` and fork.
With an arena it is two lines and runs in microseconds.

Every `ArenaAlloc` call site in the codebase checks the result. Follow one
through the JSON parser to see the discipline:

```c
// engine/src/json.c
ItemNode *node = (ItemNode *)ArenaAlloc(ps->arena, sizeof(ItemNode));
if (!node) { Fail(ps, "out of arena memory"); return false; }
```

The failure propagates as a `false` return all the way to `JsonParse`, which
returns `NULL` and fills the error string. No partial state is left behind,
because there is no partial state to leave — the arena is thrown away whole.

---

## Diagnostics

`peak` and `name` exist so the logs can tell you how close you came:

```
LEVEL: 'Ardennes Circuit' loaded — 732 props, 182 colliders, 36 sand traps,
       6 spawns, 212 waypoints, 12 checkpoints, 15 lights (56.1/62.8 KB arena)
```

That last pair is `arena.used / arena.capacity`. If the ratio ever approached
1.0 you would know `EstimateLevelBytes` was cutting it too fine. Run
`./build/desktop/racer --frames 1` and read the log to see the real numbers for
your build.

---

## Exercises

1. **Watch the estimate.** Add a `TraceLog` at the end of `LevelLoad` printing
   `level->arena.used`, `level->arena.capacity`, and the difference. Load both
   circuits. How much slack does `EstimateLevelBytes` leave? Is it proportional
   to prop count, as the `16 * (8 + propCount)` term suggests?

2. **Make it fail.** Change `EstimateLevelBytes` to `return 1024;`. What exactly
   goes wrong, and where is the first `NULL` check that catches it? Does the
   game exit cleanly or crash? (Then put it back.)

3. **Add a scoped marker.** Implement `size_t ArenaMark(Arena*)` and
   `void ArenaRelease(Arena*, size_t mark)` that save and restore `used`. What
   invariant must a caller maintain for this to be safe? Why is this pattern
   dangerous in a codebase where pointers are stored across frames? (Hint: think
   about what `Level.props` points into.)

4. **Alignment archaeology.** Change `ARENA_ALIGN` to 1 and run `make test` on
   an aarch64 machine (this Pi is one). Does it still pass? Now change the first
   field of `LevelProp` from `const char *` to `char` and try again. Explain the
   difference between "works" and "is correct" here.

5. **Count the savings.** Instrument `ArenaAlloc` with a static counter and
   print it after loading `circuit02`. That number is how many `malloc`/`free`
   pairs the arena replaced with one.

---

Next: [03 — Writing a JSON parser](03-json-parser.md)
