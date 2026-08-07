# 05 — Splines and arc length

> `engine/include/engine/spline.h` · `engine/src/spline.c` — 262 lines.
> Tests: `tests/test_spline.c`.

---

## The problem

A level gives you a list of waypoints — 212 of them for circuit01, 489 for
circuit02 — forming a closed ring around the track. From that, four different
systems need answers to four different questions, every tick, for every car:

- **The race** asks *"how far round the lap is this car?"* — to count laps and
  sort standings.
- **The physics** asks *"am I on the tarmac, and what is the gradient here?"*
- **The AI** asks *"where is the track 1.2 units ahead of me, and how sharply
  does it turn in the next 4 units?"*
- **The terrain** asks *"how high is the ground at this arbitrary point?"*

A raw polyline answers none of these well. It is not smooth (the AI would brake
at every vertex), it has no arc-length parameterisation (so "1.2 units ahead" is
not expressible), and finding the nearest point on it is O(n) per query. On
circuit02 the built spline has 1,467 samples, so a naive search would be
1,467 × 6 cars × 120 Hz ≈ 1.1 million segment projections a second — twice over,
because `RaceUpdate` queries before and after collision.

So the engine builds a **spline**: a smoothed, uniformly resampled, arc-length
indexed representation of the centre line, with per-sample width, tangent and
gradient.

---

## The data structure

```c
// engine/include/engine/spline.h
typedef struct SplineSample {
    Vector3 position;
    Vector3 tangent;        // unit and horizontal, points down-track
    float width;            // drivable half-width is width * 0.5f
    float distance;         // arc length from sample 0, measured on the ground
    float grade;            // rise over run: +0.1 climbs one unit every ten
} SplineSample;

typedef struct Spline {
    SplineSample *samples;
    int count;
    float length;           // total loop length
    Arena arena;
} Spline;
```

A flat array. The smoothing happens once, at build time; at runtime this is a
polyline again — just a much finer, better-behaved one, with precomputed
derived quantities.

That is the central design idea worth taking away: **precompute the curve into
samples and every query becomes elementary.** No Bézier evaluation at runtime,
no Newton iteration to invert arc length, no derivative calculations. Just
lookups and lerps.

`SPLINE_SPACING` in `main.c` is `0.22f`, so circuit01's 212 waypoints become
636 samples over a 95.08-unit loop, and circuit02's 489 become 1,467 over
219.13 units. At 36 bytes a sample that is 23 KB and 53 KB — nothing.

---

## Building: Catmull-Rom

### Why interpolate at all

The exported waypoints trace a polyline. Drive it and every vertex is a
discontinuity in direction. The AI, which measures curvature by differencing
tangents (Chapter 08), would see an infinite curvature spike at each one and
brake hard on a straight.

You want a curve that:

1. **Passes through** the control points (they are the racing line the author
   drew, not suggestions),
2. is **C¹ continuous** (tangent direction never jumps),
3. is **local** (moving one waypoint does not reshape the far side of the
   circuit),
4. needs **no extra data** (no tangent handles to author).

That is exactly the specification of a **Catmull-Rom spline**.

### The idea

For each span between control points `p1` and `p2`, Catmull-Rom uses the
neighbours `p0` and `p3` to estimate the tangents. The uniform form sets the
tangent at `p1` to `(p2 − p0)/2` — the direction from the point before to the
point after. Continuity is automatic because adjacent spans agree on the shared
tangent by construction.

Because it interpolates its control points and needs only the neighbours, it is
the default choice for a path through authored points.

### The centripetal variant, and why it is not optional here

```c
// engine/src/spline.c
// Centripetal Catmull-Rom, evaluated with Barry-Goldman.
//
// The uniform form assumes the control points are evenly spaced. A track's are
// not — a hand-drawn racing line, or an exporter that thins arcs harder than
// straights, leaves spans of very different lengths — and where the spacing
// jumps the uniform curve overshoots. That shows up as a phantom hairpin in the
// curvature, which the AI dutifully brakes for. Knot spacing of sqrt(distance)
// removes it, and also guarantees no cusps or self-intersections within a span.
static Vector3 CatmullRom(Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3, float t)
{
    const float alpha = 0.5f;   // centripetal
    float t0 = 0.0f;
    float t1 = t0 + powf(fmaxf(Vector3Distance(p0, p1), 1e-5f), alpha);
    float t2 = t1 + powf(fmaxf(Vector3Distance(p1, p2), 1e-5f), alpha);
    float t3 = t2 + powf(fmaxf(Vector3Distance(p2, p3), 1e-5f), alpha);

    float tt = t1 + (t2 - t1) * t;   // t arrives normalised across the middle span

    Vector3 a1 = Vector3Lerp(p0, p1, (tt - t0) / (t1 - t0));
    Vector3 a2 = Vector3Lerp(p1, p2, (tt - t1) / (t2 - t1));
    Vector3 a3 = Vector3Lerp(p2, p3, (tt - t2) / (t3 - t2));
    Vector3 b1 = Vector3Lerp(a1, a2, (tt - t0) / (t2 - t0));
    Vector3 b2 = Vector3Lerp(a2, a3, (tt - t1) / (t3 - t1));
    return Vector3Lerp(b1, b2, (tt - t1) / (t2 - t1));
}
```

The **knot parameterisation** is what `alpha` controls. Catmull-Rom needs to
assign a parameter value `tᵢ` to each control point, and the spacing between
those values is:

```
t_{i+1} - t_i = |p_{i+1} - p_i|^α
```

| α | Name | Behaviour |
|---|---|---|
| 0 | Uniform | Every span gets parameter width 1, regardless of length |
| 0.5 | **Centripetal** | Parameter width is √(distance) |
| 1 | Chordal | Parameter width is the distance |

**Why uniform fails.** Suppose three waypoints are 0.2 units apart and the next
is 5 units away. Uniform parameterisation gives all four spans the same
parameter budget, so the curve must traverse 5 units in the same "time" it
traverses 0.2. To do that with a continuous tangent it has to bulge — it
overshoots the control points, looping outside the intended path. On a racing
line that reads as an S-bend that is not in the track, and the AI's curvature
probe finds a corner where there is none.

**Why centripetal is the right α.** Lee (1989) and Yuksel et al. (2011) proved
that α = 0.5 is the unique choice guaranteeing **no cusps and no
self-intersections within a span**, for any control point configuration. α = 1
(chordal) also avoids overshoot but does not have the guarantee, and pulls the
curve tighter than usually looks right.

The half-power is not aesthetic tuning. It is the value at which a theorem
applies.

### Barry-Goldman evaluation

The nested-lerp form above is the **Barry-Goldman pyramid**, and it is a
generalisation of de Casteljau's algorithm to non-uniform knots.

```
        p0        p1        p2        p3
          \      /  \      /  \      /
           a1        a2        a3          (lerp over adjacent knot spans)
             \      /  \      /
               b1        b2                (lerp over two-span windows)
                 \      /
                  result                   (lerp over the middle span)
```

Six lerps and a handful of divisions. Compare the alternative: build the 4×4
non-uniform Catmull-Rom basis matrix and evaluate a cubic. That is more
arithmetic, far more code, and much harder to read — the pyramid is manifestly a
sequence of interpolations, so it is manifestly inside the convex hull of the
control points at every step.

**The `fmaxf(..., 1e-5f)`** guards against duplicate control points. Two
identical waypoints would give a zero-length span, hence a zero denominator in
`(tt - t1) / (t2 - t1)`. Clamping the distance from below turns a `NaN` into a
harmless near-degenerate span. Levels do sometimes contain duplicate points, and
a `NaN` position propagates into every downstream system silently.

---

## The three build passes

```c
bool SplineBuild(Spline *spline, const Level *level, float spacing)
```

### Pass 1 — measure the subdivision

```c
int total = 0;
for (int i = 0; i < n; i++) {
    Vector3 a = level->waypoints[i].position;
    Vector3 b = level->waypoints[(i + 1) % n].position;
    float chord = Vector3Distance(a, b);
    int steps = (int)ceilf(chord / spacing);
    if (steps < 1) steps = 1;
    if (steps > 512) steps = 512;
    subdiv[i] = steps;
    total += steps;
}
```

Each waypoint span is subdivided enough that the resulting samples are roughly
`spacing` apart. A long span gets more subdivisions than a short one — which is
what makes the output *uniform* even though the input is not.

`(i + 1) % n` is the closed-loop idiom, and it appears about fifteen times in
this file. The loop is implicitly closed; there is no duplicated final point.

The `512` clamp bounds the damage from a stray waypoint 10,000 units away. Both
clamps are the same defensive shape you saw in `CollisionWorldBuild` and
`TerrainBuild`: **a level file is untrusted input, and every derived size gets a
ceiling.**

### Pass 2 — evaluate

```c
int out = 0;
for (int i = 0; i < n; i++) {
    Vector3 p0 = level->waypoints[(i - 1 + n) % n].position;
    Vector3 p1 = level->waypoints[i].position;
    Vector3 p2 = level->waypoints[(i + 1) % n].position;
    Vector3 p3 = level->waypoints[(i + 2) % n].position;
    float w1 = level->waypoints[i].width;
    float w2 = level->waypoints[(i + 1) % n].width;

    for (int s = 0; s < subdiv[i]; s++) {
        float t = (float)s / (float)subdiv[i];
        spline->samples[out].position = CatmullRom(p0, p1, p2, p3, t);
        spline->samples[out].width = LerpF(w1, w2, t);
        out++;
    }
}
```

`(i - 1 + n) % n` — the backwards wrap. In C, `-1 % n` is `-1`, not `n-1`, so
you must add `n` before taking the modulus. This bug is so common it is worth
memorising the idiom.

Width is linearly interpolated rather than splined. Width is authored per
waypoint and rarely varies; a linear ramp between two values is visually
indistinguishable from a smooth one and costs one lerp.

Note `s < subdiv[i]` with `t = s / subdiv[i]`, so `t` runs `0, 1/k, ..., (k-1)/k`
and never reaches 1. The endpoint belongs to the next span. Emitting it here too
would duplicate every waypoint in the sample array.

### Pass 3 — arc length, tangents, grade

```c
// Pass 3: arc lengths, tangents and grades, all on the closed loop.
// Distances are measured on the ground plane so that lap progress, AI
// look-ahead and gate spacing do not stretch on a climb.
float acc = 0.0f;
for (int i = 0; i < total; i++) {
    spline->samples[i].distance = acc;
    Vector3 a = spline->samples[i].position;
    Vector3 b = spline->samples[(i + 1) % total].position;
    acc += sqrtf((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
}
spline->length = acc;
```

**Horizontal arc length.** Note the missing `y` term. This is a real design
decision with visible consequences.

A section that climbs 2 units over 10 horizontal units has a true 3D length of
√(10² + 2²) ≈ 10.20. Measuring it as 10.00 means:

- Lap progress does not stretch on a climb — a car "gains" no distance by going
  uphill.
- Checkpoint spacing stays even in plan view.
- The AI's `lookAhead` of 1.2 units means 1.2 units *of map*, so its behaviour
  is the same on the flat and on a gradient.

The README puts it plainly: *"Arc lengths are measured on the ground, so a climb
does not stretch lap progress or gate spacing."*

The cost is that a car's odometer would under-read on a hilly track. Nothing in
the game shows an odometer, so the cost is zero.

```c
for (int i = 0; i < total; i++) {
    Vector3 prev = spline->samples[(i - 1 + total) % total].position;
    Vector3 next = spline->samples[(i + 1) % total].position;
    Vector3 dir = Vector3Subtract(next, prev);

    float rise = dir.y;
    dir.y = 0.0f;
    float run = Vector3Length(dir);
    spline->samples[i].tangent = (run > 1e-6f) ? Vector3Scale(dir, 1.0f / run)
                                               : (Vector3){ 0, 0, 1 };
    spline->samples[i].grade = (run > 1e-6f) ? rise / run : 0.0f;
}
```

**Central differences** for the tangent: `(next − prev)` rather than
`(next − current)`. A forward difference is first-order accurate; a central
difference is second-order, and — more importantly here — it is *symmetric*, so
the tangent at a sample does not lean toward the next sample. On a corner that
matters visibly.

**Grade is rise over run**, the same convention as a road sign. `0.18` is an 18%
gradient. It is stored on the horizontal run, matching the arc length convention,
so `grade × horizontalDistance` gives the height change directly.

Both fall back to a sane default when the run is degenerate. `{0,0,1}` is the
engine's zero-yaw direction, so a degenerate sample points "north" rather than
producing a `NaN` that would poison every car that queries it.

---

## The nearest-point query

`SplineClosest` is the workhorse. It runs at least twice per car per tick — once
to determine the surface, once after collision resolution to update progress —
plus once per AI think. With 6 cars at 120 Hz that is over 2,000 calls a second,
each of which conceptually has to search up to 1,000 samples.

### Projecting onto one segment

```c
// Projects `point` onto the segment starting at sample `i`, returning the
// parameter along it clamped to [0,1] and the squared distance.
static float ProjectOnSegment(const Spline *s, int i, Vector3 point, float *outSqDist)
{
    int j = (i + 1) % s->count;
    Vector3 a = s->samples[i].position;
    Vector3 b = s->samples[j].position;
    Vector3 ab = Vector3Subtract(b, a);
    ab.y = 0.0f;
    Vector3 ap = Vector3Subtract(point, a);
    ap.y = 0.0f;

    float denom = ab.x * ab.x + ab.z * ab.z;
    float t = (denom > 1e-9f) ? (ap.x * ab.x + ap.z * ab.z) / denom : 0.0f;
    t = Clamp(t, 0.0f, 1.0f);

    float dx = ap.x - ab.x * t;
    float dz = ap.z - ab.z * t;
    *outSqDist = dx * dx + dz * dz;
    return t;
}
```

The standard point-to-segment projection. `t = (ap·ab)/(ab·ab)` is the scalar
projection of `ap` onto `ab`, normalised to the segment's length. Clamping to
[0,1] turns the infinite-line projection into a segment projection: outside the
range, the nearest point is an endpoint.

**Squared distance, never `sqrt`.** For comparison purposes `d² < e²` iff
`d < e`, so the square root is pure waste. This appears throughout the codebase
(`CollisionWorldQuery`, `SkidUpdate`, `AIThink`); it is one of the few
micro-optimisations that costs nothing in readability.

**Everything flattened to XZ.** `ab.y = 0` and `ap.y = 0`. A car's height is a
consequence of where it is on the map, not an input to finding where it is. This
is the "driving stays a 2D problem" principle from the README, made concrete.

### The two-tier search

```c
SplineQuery SplineClosest(const Spline *spline, Vector3 point, int *hintIndex)
{
    /* ... */
    int hint = (hintIndex && *hintIndex >= 0) ? *hintIndex % spline->count : -1;

    // Fine sweep around the caller's last answer, which is where a car that
    // moved normally will be.
    if (hint >= 0) {
        for (int k = -SPLINE_SEARCH_WINDOW; k <= SPLINE_SEARCH_WINDOW; k++) {
            int i = ((hint + k) % spline->count + spline->count) % spline->count;
            float sq;
            float t = ProjectOnSegment(spline, i, point, &sq);
            if (sq < bestSq) { bestSq = sq; best = i; bestT = t; }
        }
    }
```

**Temporal coherence.** A car at 6.6 u/s moving for 1/120 s travels 0.055 units.
Samples are 0.22 apart. So between ticks the nearest sample moves by at most
one index — usually zero. The caller stores its last answer in `hintIndex` and
the search checks ±24 samples around it: 49 projections instead of 1,467.

This is one of the most broadly useful optimisations in interactive software.
**If a query's answer changes slowly, cache the previous answer and search near
it.** It shows up in collision (sweep-and-prune's sorted axis), in rendering
(temporal reprojection), in physics (warm-starting constraint solvers).

The double modulus `((hint + k) % count + count) % count` handles `hint + k`
going negative. Same trap as `(i - 1 + n) % n`.

### The coarse sweep, and the bug it exists to prevent

```c
    // Coarse sweep of the whole loop. Without this a stale hint — after a
    // respawn, or on a circuit whose two halves run close together — can pin
    // the query to the wrong side of the track, and no distance threshold
    // reliably detects that. The stride keeps it cheap enough to run always.
    int stride = spline->count / 64;
    if (stride < 1) stride = 1;

    int coarseBest = -1;
    float coarseSq = bestSq;
    for (int i = 0; i < spline->count; i += stride) {
        float sq;
        float t = ProjectOnSegment(spline, i, point, &sq);
        if (sq < coarseSq) { coarseSq = sq; coarseBest = i; bestT = t; }
    }

    // Refine around the coarse winner when it beats the windowed result.
    if (coarseBest >= 0) {
        bestSq = coarseSq;
        best = coarseBest;
        for (int k = -stride; k <= stride; k++) { /* fine search around it */ }
    }
```

This is the most interesting code in the file, because it is a defence against a
specific and nasty failure.

Consider circuit01. The lap runs out and comes back; at several points the
outbound and return sections pass within a couple of units of each other. Now
suppose a car respawns, or gets shoved sideways, or the hint is simply stale
after a level reload. The ±24 window searches the *wrong* section of track. The
car's reported arc length jumps half a lap. Lap counting corrupts.

The tempting fix is a distance threshold: "if the best match is more than X
units away, do a full search". The comment rejects it, and correctly — **no
threshold works**, because when two parts of the track are 1.5 units apart, a
car 1.2 units off the wrong one looks exactly as plausible as a car 1.2 units
off the right one. The local information is genuinely insufficient.

So the coarse sweep runs **unconditionally**. It samples every `count/64`-th
segment — about 64 projections regardless of circuit size — and refines around
the winner if it beats the windowed result. On circuit02 the stride is 22, so
the total is roughly 49 + 64 + 45 ≈ 160 projections, versus 1,467 for a full
search. Correct in all cases, and still nine times cheaper.

Note it only *overrides* the windowed result when it is strictly better
(`coarseSq` starts at `bestSq`). The window is finer, so it wins ties, which is
what you want: prefer continuity when both answers are equally good.

This is worth generalising: **when a fast path can be wrong in a way you cannot
detect locally, do not add a heuristic detector. Add a cheap global check.**

### Assembling the result

```c
    int next = (best + 1) % spline->count;
    Vector3 a = spline->samples[best].position;
    Vector3 b = spline->samples[next].position;

    q.index = best;
    q.position = Vector3Lerp(a, b, bestT);      // carries the surface height
    q.tangent = spline->samples[best].tangent;
    q.halfWidth = LerpF(spline->samples[best].width, spline->samples[next].width, bestT) * 0.5f;
    q.grade = LerpF(spline->samples[best].grade, spline->samples[next].grade, bestT);

    // Horizontal, to match how sample distances were accumulated.
    float segLen = sqrtf((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
    q.distance = spline->samples[best].distance + segLen * bestT;
    if (q.distance >= spline->length) q.distance -= spline->length;

    // Sign the lateral offset using the track's right vector (tangent x up).
    Vector3 right = { -q.tangent.z, 0.0f, q.tangent.x };
    Vector3 rel = Vector3Subtract(point, q.position);
    q.lateral = rel.x * right.x + rel.z * right.z;

    if (hintIndex) *hintIndex = best;
    return q;
}
```

Seven fields, each consumed by a different subsystem:

| Field | Used by | For |
|---|---|---|
| `position` | `race.c` | surface height under the car |
| `tangent` | `race.c`, `ai.c` | which way the track runs; grade sign |
| `distance` | `race.c` | lap progress, gate crossing, standings |
| `lateral` | `race.c`, `ai.c` | on/off track, apex targeting |
| `halfWidth` | `race.c`, `ai.c` | the drivable lane |
| `grade` | `race.c`, `car.c` | gravity along the road |
| `index` | the caller | next frame's hint |

**The right vector.** `right = (−tangent.z, 0, tangent.x)` — a 90° rotation on
the XZ plane. Dotting the offset with it gives a signed lateral distance:
positive to the right of the direction of travel. The sign is what lets the AI
say "the apex is on my left" rather than just "I am 0.3 off line".

**`q.distance` wraps but never goes negative**, because `bestT ∈ [0,1]` and
`samples[best].distance < length`. The one subtraction is enough.

---

## Sampling by arc length

The AI needs "the track 1.2 units ahead". That is the inverse problem: given a
distance, find the point.

```c
SplineSample SplineSampleAt(const Spline *spline, float distance)
{
    distance = fmodf(distance, spline->length);
    if (distance < 0.0f) distance += spline->length;

    // Samples are near-uniform, so start from the proportional guess and walk.
    int i = (int)((distance / spline->length) * (float)spline->count) % spline->count;
    for (int guard = 0; guard < spline->count; guard++) {
        float d0 = spline->samples[i].distance;
        int j = (i + 1) % spline->count;
        float d1 = (j == 0) ? spline->length : spline->samples[j].distance;
        if (distance < d0) { i = (i - 1 + spline->count) % spline->count; continue; }
        if (distance > d1) { i = j; continue; }

        float t = (d1 > d0) ? (distance - d0) / (d1 - d0) : 0.0f;
        out.position = Vector3Lerp(spline->samples[i].position, spline->samples[j].position, t);
        /* ... interpolate tangent, width, grade ... */
        return out;
    }
    return spline->samples[0];
}
```

**The proportional guess.** Because pass 1 made the samples nearly evenly
spaced, `distance/length × count` lands within a sample or two of the answer.
A binary search would be O(log n) and correct; this is O(1) in practice and
simpler. It works *because* of the resampling, which is a nice example of an
earlier design decision paying off somewhere unexpected.

**`d1 = (j == 0) ? length : samples[j].distance`** is the wrap. `samples[0].distance`
is 0, so the last segment would otherwise compute `t = (distance - d0)/(0 - d0)`,
a negative denominator, and interpolate backwards across the entire loop.

**The `guard` counter** bounds the walk. If the distances were ever non-monotonic
— a corrupt spline, a `NaN` — the loop would oscillate between two indices
forever. The guard turns an infinite loop into a wrong answer, which is the
right trade in a 120 Hz update. Note the guard is checked, not asserted: this
code must not crash the game over bad level data.

---

## Wrapped differences

```c
float SplineWrapDelta(const Spline *spline, float a, float b)
{
    float half = spline->length * 0.5f;
    float d = fmodf(a - b, spline->length);
    if (d > half) d -= spline->length;
    if (d < -half) d += spline->length;
    return d;
}
```

Twelve lines that make lap counting possible.

On a 95-unit loop, a car at arc length 94.5 and a gate at 0.5 are 1.0 units
apart, not 94.0. Naive subtraction gets that catastrophically wrong exactly once
per lap — at the finish line, which is the one place it matters.

`SplineWrapDelta` returns the shortest signed difference, in
`(−length/2, +length/2]`. Chapter 09 shows how `UpdateCheckpoints` builds gate
detection on top of it as a sign change.

The same wrap-to-shortest problem appears for angles. `render.c` has the
identical function for radians:

```c
// engine/src/render.c
static float WrapAngle(float a)
{
    while (a > PI) a -= 2.0f * PI;
    while (a < -PI) a += 2.0f * PI;
    return a;
}
```

Any time you have a cyclic quantity — angle, arc length, time of day, hue —
you need this function. Write it once per space and use it everywhere.

---

## How the tests pin it down

`tests/test_spline.c` builds a synthetic level with no file at all:

```c
// Builds a level holding just a waypoint ring of the given radius.
static bool MakeRingLevel(Level *level, int count, float radius, float width)
{
    memset(level, 0, sizeof(*level));
    ArenaInit(&level->arena, sizeof(LevelWaypoint) * (size_t)count + 256, "test");
    level->waypoints = ArenaAlloc(&level->arena, sizeof(LevelWaypoint) * (size_t)count);
    level->waypointCount = count;
    /* ... place `count` points on a circle of `radius` ... */
}
```

A 32-gon of radius 10. Now every property has a closed-form expected value:

```c
// A smoothed 32-gon of radius 10 should measure close to a circle.
float circumference = 2.0f * PI * 10.0f;
CHECK(fabsf(spline.length - circumference) < circumference * 0.02f,
      "loop length %.2f, expected about %.2f", (double)spline.length, (double)circumference);
```

This is the key move for testing geometry: **choose an input whose exact answer
you know.** A circle's circumference is 2πr; its nearest-point query has an
analytic answer; its tangent is always perpendicular to the radius. You cannot
write that test against a real circuit, because you would be asserting whatever
the code currently produces.

The 2% tolerance accounts for the polygon-vs-circle difference and the smoothing.

---

## Exercises

1. **See the overshoot.** Change `alpha` in `CatmullRom` from `0.5f` to `0.0f`
   (uniform). Run `make test --verbose` and compare "tightest radius" for both
   circuits. Then run the game with `--debug` and look at the cyan centre line
   near a corner. What did centripetal parameterisation buy?

2. **Delete the coarse sweep.** Comment out the coarse loop in `SplineClosest`.
   Run `make test`. Does it still pass? Now drive circuit02 and press `R` to
   respawn repeatedly near a point where the track doubles back. Watch the lap
   counter and the minimap dot.

3. **Cost the hint.** Instrument `SplineClosest` to count `ProjectOnSegment`
   calls and print the total after a headless run
   (`--autopilot --frames 3600`). Then force `hint = -1` on every call and
   compare. How many segment projections did temporal coherence save?

4. **3D arc length.** Change pass 3 to accumulate the full 3D distance
   (include the `y` term). Run `make test`. Which assertions move, and by how
   much? Would a player notice?

5. **A cusp.** Build a ring level with two coincident waypoints (set
   `waypoints[5] = waypoints[4]`). What does `fmaxf(..., 1e-5f)` prevent? Remove
   the clamp and observe.

6. **Curvature as a first-class field.** Add `float curvature` to
   `SplineSample`, computed at build time from the turn between consecutive
   tangents divided by the distance between them. Then rewrite the AI's probe
   loop (Chapter 08) to read it instead of recomputing. Is the AI faster? Is it
   the same?

---

Next: [06 — Collision detection](06-collision.md)
