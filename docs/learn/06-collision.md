# 06 — Collision detection

> `engine/include/engine/collide.h` · `engine/src/collide.c` — 284 lines.
> Tests: `tests/test_collide.c`.

---

## The problem

A circuit has 182 solid objects (circuit01) or 300 (circuit02): barriers, trees,
grandstands, pit buildings, lamp posts. Six cars need to not drive through any
of them, at 120 Hz.

The naive answer is 6 × 300 = 1,800 pair tests per tick, 216,000 per second,
each involving trigonometry. That is survivable on a desktop and not on a Pi.
And it is *wasteful* — a car near the start line cannot possibly touch a barrier
at Stavelot.

So collision detection splits into two stages, as it does in every engine:

- **Broad phase** — cheaply reject pairs that cannot possibly touch. Here: a
  uniform grid.
- **Narrow phase** — exactly test the survivors, and report how to separate
  them. Here: the separating axis theorem on oriented boxes.

---

## Why 2D

```c
// engine/include/engine/collide.h
// Collision on the XZ plane. A top-down racer never needs the Y axis for
// resolution, so everything here is 2D: oriented boxes solved with SAT, plus a
// uniform grid over the level's static colliders.
```

The game has elevation — circuit01 climbs 2.25 units, circuit02 more than
double that. But cars never leave the ground, never jump, and never drive under
anything. Height is a *consequence* of position (looked up from the spline), not
a degree of freedom.

Dropping a dimension is not a small saving. It is:

- 4 candidate axes for SAT instead of 15,
- a 2D grid instead of a 3D one (which for the same cell size would be roughly
  40× the cells here),
- no rotation about anything but yaw, so a full orientation is one float,
- no gravity integration, no ground contact resolution, no resting-contact
  jitter.

`LevelCollider` still carries a `height`, and `CollisionWorld` copies it into its
own `heights` array — but no collision query ever reads it. The only thing that
consumes a height is `RenderDebugColliders`, which draws the wireframe boxes for
the `F1` overlay. It is otherwise there for a future in which something can pass
over a low barrier: a defensible amount of speculative data, one float per
collider, carried through the format and ignored.

**Look hard for a dimension you can drop.** It is usually the single largest
simplification available in a physics system.

---

## Representing an oriented box

```c
typedef struct Obb2 {
    Vector2 center;         // (x, z)
    Vector2 halfExtents;
    float yaw;              // radians, about +Y
} Obb2;
```

Five floats. Note `Vector2` here holds `(x, z)` — the vertical axis is simply
absent, so raylib's `Vector2.y` field carries the world's `z`. That reuse is
slightly uncomfortable to read and completely free at runtime; the header states
the convention and every function honours it.

**Half extents rather than full size** is the standard choice. Almost every
formula wants the half — the extent from the centre — so storing it avoids a
multiply-by-0.5 at every use.

### The axes

```c
static void Obb2Axes(Obb2 b, Vector2 *ux, Vector2 *uz)
{
    float c = cosf(b.yaw), s = sinf(b.yaw);
    // Yaw about +Y with the engine's left-handed XZ convention.
    *ux = (Vector2){ c, -s };
    *uz = (Vector2){ s, c };
}
```

The box's local X and Z axes in world space. Note the sign pattern: `ux = (c, −s)`
and `uz = (s, c)`. A textbook 2D rotation gives `(c, −s)` and `(s, c)` for a
*clockwise* rotation, which is what a yaw about +Y looks like when you are
looking down the +Y axis at the XZ plane.

This convention must match `CarForward`/`CarRight` in `game/src/car.c` and
`LevelInSandtrap` in `engine/src/level.c` exactly, or a sand trap and a barrier
authored with the same yaw would cover different ground. `level.h` says so
explicitly. Sign conventions are the most common source of "it works but
everything is mirrored" bugs, and the only defence is writing them down once and
citing that note everywhere.

### Corners and point containment

```c
void Obb2Corners(Obb2 box, Vector2 out[4])
{
    Vector2 ux, uz;
    Obb2Axes(box, &ux, &uz);
    Vector2 ex = { ux.x * box.halfExtents.x, ux.y * box.halfExtents.x };
    Vector2 ez = { uz.x * box.halfExtents.y, uz.y * box.halfExtents.y };
    out[0] = (Vector2){ box.center.x - ex.x - ez.x, box.center.y - ex.y - ez.y };
    out[1] = (Vector2){ box.center.x + ex.x - ez.x, box.center.y + ex.y - ez.y };
    out[2] = (Vector2){ box.center.x + ex.x + ez.x, box.center.y + ex.y + ez.y };
    out[3] = (Vector2){ box.center.x - ex.x + ez.x, box.center.y - ex.y + ez.y };
}
```

Centre ± half-extent along each axis, in the four sign combinations, ordered so
the corners walk the perimeter rather than crossing diagonally. Used by the
skid-mark shadow quad in `main.c` and by the sand-trap tests.

```c
bool Obb2ContainsPoint(Obb2 box, Vector2 point)
{
    Vector2 ux, uz;
    Obb2Axes(box, &ux, &uz);
    Vector2 d = { point.x - box.center.x, point.y - box.center.y };
    float px = d.x * ux.x + d.y * ux.y;
    float pz = d.x * uz.x + d.y * uz.y;
    return fabsf(px) <= box.halfExtents.x && fabsf(pz) <= box.halfExtents.y;
}
```

Project the offset onto each local axis, compare against the half extent. This
is the same technique as `LevelInSandtrap` from Chapter 04 — transform the point
into the box's frame, then do the trivial axis-aligned test.

---

## The separating axis theorem

### The theorem

> Two convex shapes are disjoint **if and only if** there exists a line (an
> "axis") onto which their projections do not overlap.

The "if" direction is obvious: separated projections mean separated shapes. The
"only if" direction — that disjoint convex shapes *always* have such an axis —
is the content of the theorem, and follows from the hyperplane separation
theorem for convex sets.

The practical version, for polygons:

> If a separating axis exists, one perpendicular to an edge of one of the two
> polygons is separating.

So instead of testing infinitely many directions, you test the edge normals. For
two rectangles that is four candidates — and because opposite edges of a
rectangle are parallel, really only two per box.

### The implementation

```c
Manifold CollideObb2(Obb2 a, Obb2 b)
{
    Manifold m = { 0 };
    Vector2 delta = { b.center.x - a.center.x, b.center.y - a.center.y };

    Vector2 axes[4];
    Obb2Axes(a, &axes[0], &axes[1]);
    Obb2Axes(b, &axes[2], &axes[3]);

    float bestDepth = 1e30f;
    Vector2 bestAxis = { 1.0f, 0.0f };

    for (int i = 0; i < 4; i++) {
        Vector2 axis = axes[i];
        float len = sqrtf(axis.x * axis.x + axis.y * axis.y);
        if (len < 1e-6f) continue;
        axis.x /= len; axis.y /= len;

        float overlap = ProjectRadius(a, axis) + ProjectRadius(b, axis) -
                        fabsf(delta.x * axis.x + delta.y * axis.y);
        if (overlap <= 0.0f) return m;      // separating axis found

        if (overlap < bestDepth) {
            bestDepth = overlap;
            // Orient the axis so it always points from a towards b.
            float sign = (delta.x * axis.x + delta.y * axis.y) < 0.0f ? -1.0f : 1.0f;
            bestAxis = (Vector2){ axis.x * sign, axis.y * sign };
        }
    }

    m.hit = true;
    m.normal = bestAxis;
    m.depth = bestDepth;
    return m;
}
```

The projection radius:

```c
// Half-width of `b` projected onto unit axis `axis`.
static float ProjectRadius(Obb2 b, Vector2 axis)
{
    Vector2 ux, uz;
    Obb2Axes(b, &ux, &uz);
    return fabsf((axis.x * ux.x + axis.y * ux.y)) * b.halfExtents.x +
           fabsf((axis.x * uz.x + axis.y * uz.y)) * b.halfExtents.y;
}
```

**Why this formula works.** The projection of a box onto an axis is an interval
centred on the projection of its centre. Its half-width is the sum, over the
box's own axes, of `|axis · localAxis| × halfExtent`. The absolute value is what
makes it a *radius* — you want the extent in both directions, so the corner
furthest along the axis, whichever corner that is.

**Overlap on an axis.** Two intervals centred at `cₐ` and `c_b` with radii `rₐ`
and `r_b` overlap iff `|c_b − cₐ| < rₐ + r_b`. Rearranged, the overlap amount is
`rₐ + r_b − |Δ·axis|`. Positive means overlapping; zero or negative means
separated.

**Early exit.** `if (overlap <= 0.0f) return m;` with `m.hit` still false. This
is the crucial performance property of SAT: in the overwhelmingly common case
where the boxes are apart, the very first axis usually proves it and the function
returns after one projection.

### The minimum translation vector

The loop does not stop at "they overlap". It tracks the axis with the **smallest**
overlap:

```c
if (overlap < bestDepth) { bestDepth = overlap; /* ... */ }
```

That axis and depth are the **minimum translation vector** — the shortest push
that separates the pair. Moving `a` by `−normal × depth` puts the boxes exactly
touching.

Why the smallest? Because you want the least disruptive correction. A car
half-overlapping a barrier could be pushed out sideways (a small move) or
lengthwise (a huge one). The small move is what looks like a car scraping a
wall; the large one looks like a teleport.

**Normal orientation.** `sign` flips the axis so it always points from `a`
toward `b`. Without this, the returned normal's direction would depend on how
the box happened to be yawed, and the caller could not know which way to push.
The header states the contract:

```c
typedef struct Manifold {
    bool hit;
    Vector2 normal;         // unit, points from `a` towards `b`
    float depth;            // penetration along `normal`
} Manifold;
```

And `tests/test_collide.c` pins it:

```c
Manifold m = CollideObb2(Box(0, 0, 1, 1, 0), Box(1.5f, 0, 1, 1, 0));
CHECK(m.hit, "overlapping boxes register");
CHECK(fabsf(m.depth - 0.5f) < 1e-4f, "penetration %.4f, expected 0.5", (double)m.depth);
CHECK(m.normal.x > 0.99f, "normal should point along +X, got (%.2f, %.2f)", /* ... */);

// The normal always points from the first box towards the second.
Manifold flipped = CollideObb2(Box(1.5f, 0, 1, 1, 0), Box(0, 0, 1, 1, 0));
CHECK(flipped.normal.x < -0.99f, "reversed normal should point along -X, got %.2f", /* ... */);
```

Two unit boxes with centres 1.5 apart overlap by exactly 0.5 — an arithmetic
answer you can check by hand, which is what makes the test meaningful.

The rotated cases are the ones that would pass with an AABB test and should not:

```c
// A 45-degree square's corner reaches further than its half extent.
CHECK(CollideObb2(Box(0, 0, 1, 1, 0), Box(2.2f, 0, 1, 1, 45)).hit, /* ... */);
```

A unit square rotated 45° reaches √2 ≈ 1.414 along X, not 1.0. Two of them with
centres 2.2 apart overlap. Get `ProjectRadius` wrong and this test fails while
everything axis-aligned still passes.

---

## Broad phase: a uniform grid

### The idea

Divide space into fixed-size cells. Record which boxes touch which cells. To
query a region, look only at the boxes in the overlapping cells.

Alternatives are a BVH (better for wildly varying object sizes, more complex to
build), a quadtree (adaptive, pointer-chasing), or sort-and-sweep (great for
mostly-1D distributions). A uniform grid wins here because the objects are all
roughly the same size (barriers and trees, none enormous), the distribution is
reasonably even (scattered along a track), and the structure is **static** —
built once at load, never updated.

### Compressed sparse row layout

```c
typedef struct CollisionWorld {
    Obb2 *boxes;
    float *heights;
    int boxCount;

    // Uniform grid in compressed-row form: cellStart[c]..cellStart[c+1] indexes
    // into cellItems, which holds indices into `boxes`.
    int *cellStart;
    int *cellItems;
    int gridW, gridH;
    float cellSize;
    Vector2 origin;         // world position of cell (0,0)

    Arena arena;
} CollisionWorld;
```

The obvious representation is an array of `std::vector<int>` — one growable list
per cell. That is `gridW × gridH` separate allocations, each with its own header,
scattered across the heap.

CSR flattens it into two arrays:

```
cellStart: [0, 0, 2, 2, 5, 6, ...]     one entry per cell, plus a terminator
cellItems: [7, 12, 3, 7, 19, 4, ...]   box indices, grouped by cell
```

Cell `c`'s contents are `cellItems[cellStart[c] .. cellStart[c+1]-1]`. Empty
cells have `cellStart[c] == cellStart[c+1]`. Two allocations total, both
contiguous, both cache-friendly to walk.

This is the same layout used for sparse matrices, and for the same reasons.

### Building it: the counting sort

```c
    world->cellStart = ArenaAlloc(&world->arena, sizeof(int) * (size_t)(cells + 1));

    // Pass 1: count how many boxes touch each cell.
    for (int i = 0; i < count; i++) {
        int x0, y0, x1, y1;
        CellRangeForBox(world, world->boxes[i], &x0, &y0, &x1, &y1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                world->cellStart[y * world->gridW + x + 1]++;
    }
    for (int c = 0; c < cells; c++) world->cellStart[c + 1] += world->cellStart[c];

    int totalItems = world->cellStart[cells];
    world->cellItems = ArenaAlloc(&world->arena, sizeof(int) * (size_t)(totalItems > 0 ? totalItems : 1));

    // Pass 2: fill, using a moving cursor per cell.
    int *cursor = ArenaAlloc(&world->arena, sizeof(int) * (size_t)cells);
    memcpy(cursor, world->cellStart, sizeof(int) * (size_t)cells);
    for (int i = 0; i < count; i++) {
        int x0, y0, x1, y1;
        CellRangeForBox(world, world->boxes[i], &x0, &y0, &x1, &y1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                world->cellItems[cursor[y * world->gridW + x]++] = i;
    }
```

This is a **counting sort**, and it is worth recognising because the pattern
recurs constantly.

1. Count occurrences per bucket — but write the count into `cellStart[c+1]`, one
   slot to the right.
2. Prefix-sum in place. Because of the offset, `cellStart[c]` now holds the
   *start* offset of cell `c` and `cellStart[c+1]` its end. The off-by-one
   shifted write is what makes the prefix sum produce starts rather than ends.
3. Copy the starts into a `cursor` array and scatter, incrementing each cursor
   as you place an item.

Three linear passes, no allocation per cell, exact sizing. The same shape
appears in `StaticBatchBuild` (count vertices, allocate, emit) and in
`SplineBuild` (count subdivisions, allocate, evaluate).

### Bounding a rotated box for the grid

```c
static void CellRangeForBox(const CollisionWorld *w, Obb2 box, int *x0, int *y0, int *x1, int *y1)
{
    // Bound the rotated box with an AABB, which is all the grid needs.
    Vector2 corners[4];
    Obb2Corners(box, corners);
    float minX = corners[0].x, maxX = corners[0].x;
    /* ... min/max over all four ... */
    *x0 = (int)floorf((minX - w->origin.x) / w->cellSize);
    /* ... clamp to [0, gridW-1] ... */
}
```

The grid does not need to know the box is rotated. Over-covering costs a few
extra candidate pairs, which the narrow phase then rejects exactly. Under-covering
would miss collisions. **When approximating for a broad phase, always err
outward.**

`floorf` rather than a cast to `int`, because C truncates toward zero: `(int)(-0.5)`
is 0 but `floorf(-0.5)` is −1. For coordinates left of the origin, truncation
would fold two cells into one and silently lose collisions. This is a classic
bug in grid code.

### Guarding against absurd grids

```c
#define COLLIDE_MAX_GRID_CELLS (1 << 20)

    // Guard against a stray collider at a huge coordinate blowing up the grid.
    while ((long long)world->gridW * world->gridH > COLLIDE_MAX_GRID_CELLS) {
        world->cellSize *= 2.0f;
        cellSize = world->cellSize;
        world->gridW = world->gridW / 2 + 1;
        world->gridH = world->gridH / 2 + 1;
    }
```

One collider at `x = 1e6` — a typo, a mis-scaled Blender object — would make the
world bounds a million units wide. At a 2-unit cell size that is 500,000 cells
per side, 2.5 × 10¹¹ cells, and a `calloc` that either fails or thrashes.

The loop doubles the cell size until the grid fits in a million cells. The
result is a coarse grid, so queries return more candidates and run slower — but
the game *runs*, and the log tells you the grid is strange:

```
COLLIDE: 182 boxes in 24x25 grid (267 refs, 10.0 KB)
```

`(long long)` on the multiply matters: `gridW * gridH` as `int` overflows and
can go negative, making the loop condition false and defeating the guard
entirely.

`TerrainBuild` has the identical guard with `TERRAIN_MAX_SAMPLES`. Two
independent systems, same defensive shape, because both derive an allocation
size from level data.

### Handling the empty case

```c
    if (count <= 0) {
        // An empty world is legal: queries simply return nothing.
        world->gridW = world->gridH = 1;
        if (!ArenaInit(&world->arena, 256, "collision")) return false;
        world->cellStart = ArenaAlloc(&world->arena, sizeof(int) * 2);
        return true;
    }
```

A level with no colliders is valid — the ring levels in `tests/test_spline.c`
have none. Rather than special-casing `NULL` at every query site, the build
produces a valid empty world. **Make the degenerate case a normal case.**

---

## Querying

```c
int CollisionWorldQuery(const CollisionWorld *world, Vector2 center, float radius,
                        int *out, int maxOut)
{
    if (world->boxCount <= 0 || maxOut <= 0) return 0;

    int x0 = (int)floorf((center.x - radius - world->origin.x) / world->cellSize);
    /* ... other three bounds, clamped ... */

    int found = 0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int c = y * world->gridW + x;
            for (int k = world->cellStart[c]; k < world->cellStart[c + 1]; k++) {
                int id = world->cellItems[k];
                // A box spanning several cells appears more than once.
                bool seen = false;
                for (int j = 0; j < found; j++) {
                    if (out[j] == id) { seen = true; break; }
                }
                if (seen) continue;
                out[found++] = id;
                if (found >= maxOut) return found;
            }
        }
    }
    return found;
}
```

**Deduplication by linear scan.** A grandstand spanning four cells is listed in
all four, and a query overlapping all four would return it four times. The fix
here is an O(found²) scan of what has been collected so far.

That looks bad and is not. A car's query radius is about 0.33 units against a
2-unit cell size, so it touches at most 4 cells and `found` is typically 0–3,
occasionally up to a dozen. The quadratic term is a handful of integer
comparisons that stay entirely in L1 cache.

The alternatives — a generation-stamped visited array, or sorting the output —
would each cost more (memory traffic, or an actual sort) than the scan they
replace, at these counts. **Asymptotically worse can be practically faster at
small n, and physics broad phases live at small n.**

The `maxOut` cap makes the function total: it never writes past the caller's
buffer, and returns early if it fills up. Silently dropping candidates in an
absurdly crowded spot is better than a buffer overrun.

---

## Resolution

```c
Vector2 CollisionResolveStatic(const CollisionWorld *world, Obb2 *box, int iterations,
                               Vector2 *outDeepestNormal, float *outDeepestDepth)
{
    Vector2 totalPush = { 0 };
    float deepest = 0.0f;
    Vector2 deepestNormal = { 0 };

    if (world->boxCount > 0) {
        int candidates[128];
        float radius = sqrtf(box->halfExtents.x * box->halfExtents.x +
                             box->halfExtents.y * box->halfExtents.y);

        for (int pass = 0; pass < iterations; pass++) {
            int n = CollisionWorldQuery(world, box->center, radius, candidates,
                                        (int)(sizeof candidates / sizeof candidates[0]));
            bool anyHit = false;
            for (int k = 0; k < n; k++) {
                Manifold m = CollideObb2(*box, world->boxes[candidates[k]]);
                if (!m.hit) continue;
                anyHit = true;
                // Normal points box -> obstacle, so push back along -normal.
                box->center.x -= m.normal.x * m.depth;
                box->center.y -= m.normal.y * m.depth;
                totalPush.x -= m.normal.x * m.depth;
                totalPush.y -= m.normal.y * m.depth;
                if (m.depth > deepest) {
                    deepest = m.depth;
                    deepestNormal = (Vector2){ -m.normal.x, -m.normal.y };
                }
            }
            if (!anyHit) break;
        }
    }
    /* ... write out params, return totalPush ... */
}
```

### The bounding radius

`radius = |halfExtents|` — the box's circumradius, the distance from centre to
corner. Using this rather than the larger half-extent means the query disc
definitely contains the box in any orientation. Erring outward again.

### Why iterate

Pushing a car out of one barrier can push it into the next. Corners are the
common case: two barriers meeting at 90°, and the fix for one violates the
other.

Three iterations (`race.c` passes 3) is the usual practical number. Each pass
re-queries — the box has moved, so its candidate set may have changed — and the
loop exits early the moment a pass finds nothing.

This is **iterative impulse resolution** in its simplest form. Full physics
engines run 4–20 iterations of a much more elaborate constraint solver; the
principle is identical, which is that a set of simultaneous constraints is
solved approximately by relaxation rather than exactly.

**No convergence guarantee.** A car crushed between two barriers less than a car
wide apart will oscillate. In practice the exporter guarantees `BARRIER_CLEARANCE`
of 0.68 around the track, and `RACE_STUCK_RESPAWN` (Chapter 09) rescues anything
that still gets wedged. Layered defences rather than a perfect solver.

### The deepest contact

`outDeepestNormal` and `outDeepestDepth` report the most significant contact of
all passes, which is what the caller uses to decide how much speed to scrub:

```c
// game/src/race.c
Obb2 box = CarBox(&racer->car, &race->tuning);
Vector2 normal = { 0 };
float depth = 0.0f;
Vector2 push = CollisionResolveStatic(race->collision, &box, 3, &normal, &depth);
if (depth > 0.0f) {
    racer->car.position = box.center;
    CarApplyContact(&racer->car, normal, 0.15f, 0.22f);
}
```

The separation of concerns here is deliberate: `collide.c` knows about geometry
and produces *positions*; `car.c` knows about vehicles and consumes *normals* to
produce *velocities*.

```c
// game/src/car.c
void CarApplyContact(Car *car, Vector2 normal, float restitution, float scrub)
{
    /* ... normalise ... */
    float into = car->velocity.x * normal.x + car->velocity.y * normal.y;
    if (into >= 0.0f) return;   // already moving away from the surface

    // Remove the approaching component (plus a little bounce) and scrub the
    // sliding component so scraping a wall costs momentum.
    float remove = into * (1.0f + restitution);
    car->velocity.x -= normal.x * remove;
    car->velocity.y -= normal.y * remove;
    car->velocity.x *= (1.0f - scrub);
    car->velocity.y *= (1.0f - scrub);
    /* ... */
}
```

Decompose velocity into the component along the normal and everything else.
Remove the normal component times `(1 + restitution)` — with `restitution = 0`
that stops the approach exactly; with `0.15` it reverses 15% of it as a small
bounce. Then multiply everything by `(1 − scrub)` so grazing a wall costs 22% of
your speed.

**The `if (into >= 0.0f) return;` guard is essential.** Without it, a car already
moving away from a wall — because a previous iteration or another contact
already fixed it — gets an impulse *toward* the wall, and boxes stick to each
other or vibrate. Every impulse-based contact solver has this check.

---

## Car-versus-car

Dynamic pairs are handled separately in `race.c`, because they need equal-and-
opposite treatment rather than one-sided pushing:

```c
// game/src/race.c
static void ResolveCarCollisions(Race *race)
{
    for (int i = 0; i < race->racerCount; i++) {
        for (int j = i + 1; j < race->racerCount; j++) {
            Car *a = &race->racers[i].car;
            Car *b = &race->racers[j].car;

            Manifold m = CollideObb2(CarBox(a, &race->tuning), CarBox(b, &race->tuning));
            if (!m.hit) continue;

            // Equal masses: split the separation and swap the closing velocity.
            float push = m.depth * 0.5f;
            a->position.x -= m.normal.x * push;
            a->position.y -= m.normal.y * push;
            b->position.x += m.normal.x * push;
            b->position.y += m.normal.y * push;

            Vector2 rel = { b->velocity.x - a->velocity.x, b->velocity.y - a->velocity.y };
            float closing = rel.x * m.normal.x + rel.y * m.normal.y;
            if (closing >= 0.0f) continue;

            float impulse = -closing * 0.55f;   // partially inelastic
            a->velocity.x -= m.normal.x * impulse;
            a->velocity.y -= m.normal.y * impulse;
            b->velocity.x += m.normal.x * impulse;
            b->velocity.y += m.normal.y * impulse;
        }
    }
}
```

No broad phase — `j = i + 1` over at most 8 cars is 28 pairs, cheaper to test
than to filter.

**Equal masses** simplify everything. Each car takes half the separation and an
equal-and-opposite impulse. With unequal masses you would weight by `1/m` and
divide by the sum of inverse masses; here every car is identical, so the weights
are ½ each and vanish into the constant.

The `0.55` coefficient is a partially inelastic restitution. `1.0` would be a
perfectly elastic bounce (billiard balls); `0.0` would have them stick.

Same `if (closing >= 0.0f) continue;` guard as before, for the same reason.

**Ordering matters.** `RaceUpdate` does static collision *per car inside* the
main loop, then car-versus-car *after* every car has moved. That ordering means
a car pushed by another can end up inside a barrier for one tick — resolved on
the next. At 120 Hz and with 0.05-unit-per-tick movement, that is invisible.
Doing it "properly" would mean a global constraint solve over all bodies and all
contacts, which is a different and much larger program.

---

## Exercises

1. **Cost the broad phase.** Instrument `CollideObb2` with a static counter.
   Run `--autopilot --frames 3600` and note the total. Then bypass the grid — in
   `CollisionResolveStatic`, test all `world->boxCount` boxes directly. Compare.

2. **Tune the cell size.** `main.c` builds the world with `cellSize = 2.0f`. Try
   0.5, 1.0, 4.0, 16.0. For each, log `gridW`, `gridH`, `totalItems` (the "refs"
   in the log line) and arena bytes, plus the narrow-phase count from exercise 1.
   Plot the tradeoff. Where is the minimum, and why is the curve U-shaped?

3. **Break the normal contract.** Delete the `sign` flip in `CollideObb2`. Which
   test in `tests/test_collide.c` fails? Then drive into a barrier and describe
   what the car does.

4. **Add circles.** Implement `CollideCircleObb2`. What is the axis set? (Hint:
   the box's two axes, plus one more that depends on where the circle is.) Why
   does the "test edge normals" shortcut not suffice for a curved shape?

5. **Restitution feel.** In `race.c`, change `CarApplyContact(..., 0.15f, 0.22f)`
   to `(0.9f, 0.0f)` and drive into a barrier. Then `(0.0f, 0.8f)`. Describe each
   in words a designer would use.

6. **Continuous collision.** At 120 Hz a car covers 0.055 units per tick and a
   barrier is 0.12 thick, so nothing tunnels. Compute the frame rate at which it
   would. Then implement a swept test: before moving, check the segment from old
   to new centre against each candidate box.

7. **Delete the dedup.** Remove the `seen` scan from `CollisionWorldQuery`. Does
   anything break? Explain why the answer is "not visibly", and what it costs.

---

Next: [07 — Vehicle physics](07-vehicle-physics.md)
