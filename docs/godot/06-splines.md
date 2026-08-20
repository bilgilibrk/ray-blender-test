# 06 — Splines and arc length

> `scripts/engine/track_spline.gd` · `scripts/engine/spline_query.gd`.
> Mirrors [C chapter 05](../learn/05-splines.md).

---

## The problem

A circuit is a closed loop with a width. Almost every question the game asks is
really a question about that loop:

- **Am I on the track?** How far sideways am I from the centre line, and is that
  more than the half-width here?
- **How far round am I?** Arc length from the finish line — which is what makes
  lap counting immune to a car being shoved sideways through a gate.
- **Who is ahead?** Compare two arc lengths, wrapping correctly at the line.
- **Where should the AI aim?** A point 1.4 units further along the loop.
- **How much does the road bend in the next four units?** Sample the tangent at
  several points ahead and add up the turn.
- **What is the gradient under this car?** Rise over run at this arc length.

Waypoints alone answer none of these. A ring of 489 positions gives you no
tangent, no arc length, no width between points, and no way to find the nearest
point without testing all 489.

So you need a **resampled, arc-length-parameterised centre line**, and every
query above becomes cheap.

---

## What Godot gives you: `Curve3D`

Godot ships a spline type, and it is genuinely good. Before writing your own it
is worth knowing exactly what it does and does not do.

```gdscript
var curve: Curve3D = path.curve
curve.bake_interval = 0.2                   # world units between baked points
var length: float = curve.get_baked_length()
var p: Vector3 = curve.sample_baked(3.5)    # point at arc length 3.5
var offset: float = curve.get_closest_offset(car_position)
var nearest: Vector3 = curve.get_closest_point(car_position)
var up: Vector3 = curve.sample_baked_up_vector(3.5)
var poly: PackedVector3Array = curve.tessellate(5, 4.0)
```

**What it gives you, free and in C++:**

- Cubic Bézier interpolation with editor-editable in/out handles per point.
- A baked polyline at a spacing you choose, and `sample_baked` by arc length —
  the exact parameterisation this chapter is about.
- `get_closest_offset` and `get_closest_point`.
- A `Path3D` gizmo in the editor, and `PathFollow3D` if you want something to
  drive along it.

**What it does not give you, all of which this game needs:**

| Needed | `Curve3D` |
|---|---|
| Drivable width per point | No. One curve, no attributes |
| Signed lateral offset (left/right of centre) | No — only the nearest point |
| Grade (rise over run) | No |
| A search hint, so a per-tick query is O(window) | No. `get_closest_offset` scans every baked point |
| Wrapped difference between two offsets | No |
| Centripetal Catmull-Rom through imported waypoints | No. Bézier handles, which imported waypoints do not have |

The last two rows are the decisive ones.

**On the search cost.** `get_closest_offset` walks the whole baked polyline.
`circuit02` at a 0.25 spacing is about 4,000 points. Six cars asking twice per
tick at 120 Hz is 4,000 × 6 × 2 × 120 ≈ **5.8 million point-segment tests per
second**. It is C++, so it will not melt a desktop, but it is by far the largest
single cost in the simulation, and it is completely unnecessary: a car that
moved 5 cm since last tick is within a couple of samples of where it was. The C
engine's windowed search reduces this to 49 tests per query. That is a
100× reduction from one idea.

**On the interpolation.** A `Curve3D` built from imported waypoints has no
handles, so `Curve3D` treats it as a polyline unless you synthesise them —
and synthesising Bézier handles that behave like a Catmull-Rom through unevenly
spaced points is more work than implementing Catmull-Rom.

So: **use `Curve3D` for authoring (pipeline B in chapter 05, where a designer
drags handles in the editor), and build your own sampled table for queries.**
The table can be built from either source. That is what `TrackSpline` is.

---

## The data structure

```gdscript
# scripts/engine/track_spline.gd
class_name TrackSpline
extends RefCounted

## Closed centre line resampled at a roughly uniform spacing, which makes
## arc-length queries — lap progress, AI look-ahead, off-track tests — a walk
## over line segments.
##
## Parallel packed arrays rather than an Array of records: 4,000 samples, five
## fields each, contiguous and cache-friendly, and directly passable to the
## GDExtension in chapter 16 with no conversion. See chapter 03.

var positions: PackedVector3Array = []
var tangents: PackedVector3Array = []    ## unit, horizontal, points down-track
var widths: PackedFloat32Array = []      ## drivable half-width is width * 0.5
var distances: PackedFloat32Array = []   ## arc length from sample 0, on the ground
var grades: PackedFloat32Array = []      ## rise over run: +0.1 climbs one in ten

var count: int = 0
var length: float = 0.0                  ## total loop length

## How far either side of the hint a windowed nearest-sample search looks.
const SEARCH_WINDOW: int = 24
```

Five arrays and two scalars. Note what is *not* here: no reference to the level,
no node, no `_process`. A `TrackSpline` is derived data with a single owner, and
chapter 03's rules make its lifetime trivial.

`SplineQuery` is its companion:

```gdscript
# scripts/engine/spline_query.gd
class_name SplineQuery
extends RefCounted

## Result of a nearest-point query. Reused per car rather than allocated per
## call — see TrackSpline.closest_into().
var position: Vector3 = Vector3.ZERO   ## closest point, including height
var tangent: Vector3 = Vector3.ZERO    ## horizontal
var distance: float = 0.0              ## arc length there — lap progress
var lateral: float = 0.0               ## signed offset from centre, + is right
var half_width: float = 0.0            ## drivable half-width there
var grade: float = 0.0                 ## slope of the track under this point
var index: int = 0                     ## sample the result came from
```

---

## Building: centripetal Catmull-Rom

### Why interpolate at all

`circuit02` has 489 waypoints over a loop of roughly 120 units — a point every
quarter of a unit, which is dense. Why not use them directly?

Because the exporter thins them: a straight needs three points and a hairpin
needs forty. Consecutive spans differ in length by a factor of ten. Walking that
raw polyline gives you a tangent that jumps discontinuously at every waypoint,
which the AI reads as curvature, which it brakes for. Interpolation is what
turns a hand-drawn ring into a smooth line whose derivative is meaningful.

### The centripetal variant, and why it is not optional

Standard (uniform) Catmull-Rom assumes evenly spaced control points. Where the
spacing jumps — exactly the pattern above — the uniform curve **overshoots**,
bulging outside the control polygon. On a racing line that shows up as a phantom
hairpin in the curvature that the AI dutifully brakes for, on a section of track
that is visibly straight.

The fix is to space the knots by `distance^α` with α = 0.5 rather than
uniformly. That is the *centripetal* parameterisation, and it comes with a proof
that no cusps or self-intersections can occur inside a span.

```gdscript
# scripts/engine/track_spline.gd
## Centripetal Catmull-Rom, evaluated with Barry-Goldman.
##
## The uniform form assumes evenly spaced control points. A track's are not — a
## hand-drawn racing line, or an exporter that thins arcs harder than straights,
## leaves spans of very different lengths — and where the spacing jumps the
## uniform curve overshoots. That shows up as a phantom hairpin in the
## curvature, which the AI brakes for. Knot spacing of sqrt(distance) removes
## it, and guarantees no cusps or self-intersections within a span.
static func _catmull_rom(p0: Vector3, p1: Vector3, p2: Vector3, p3: Vector3,
                         t: float) -> Vector3:
    const ALPHA: float = 0.5      # centripetal
    var t0: float = 0.0
    var t1: float = t0 + pow(maxf(p0.distance_to(p1), 1e-5), ALPHA)
    var t2: float = t1 + pow(maxf(p1.distance_to(p2), 1e-5), ALPHA)
    var t3: float = t2 + pow(maxf(p2.distance_to(p3), 1e-5), ALPHA)

    var tt: float = t1 + (t2 - t1) * t     # t arrives normalised across the middle span

    var a1: Vector3 = p0.lerp(p1, (tt - t0) / (t1 - t0))
    var a2: Vector3 = p1.lerp(p2, (tt - t1) / (t2 - t1))
    var a3: Vector3 = p2.lerp(p3, (tt - t2) / (t3 - t2))
    var b1: Vector3 = a1.lerp(a2, (tt - t0) / (t2 - t0))
    var b2: Vector3 = a2.lerp(a3, (tt - t1) / (t3 - t1))
    return b1.lerp(b2, (tt - t1) / (t2 - t1))
```

**Barry-Goldman** is the nested-lerp evaluation: three lerps, then two, then
one. It is slower than expanding the polynomial into a basis-matrix form, and it
is used here because it is *readable* — you can see the pyramid — and because it
never divides by a knot difference that could be zero, given the `maxf(…, 1e-5)`
floors.

The `maxf` floors matter. Two coincident waypoints — which an exporter will
eventually produce — make `t1 == t0`, and the first lerp divides by zero. In C
that is a NaN that propagates silently into the whole spline. In GDScript it is
a NaN that propagates silently into the whole spline. Neither language saves
you; the floor does.

---

## The three build passes

The two-pass allocation shape from chapter 03: **measure, allocate once, fill.**

```gdscript
func build_from_level(level: LevelData, spacing: float = 0.25) -> bool:
    var n: int = level.waypoint_count()
    if n < 3:
        push_error("SPLINE: level '%s' has %d waypoints, need at least 3"
                % [level.display_name, n])
        return false
    if spacing <= 0.0001:
        spacing = 0.25

    var points: PackedVector3Array = level.waypoint_positions
    var track_widths: PackedFloat32Array = level.waypoint_widths

    # --- pass 1: how many samples does each span need? --------------------
    var subdiv: PackedInt32Array = []
    subdiv.resize(n)
    var total: int = 0
    for i: int in n:
        var chord: float = points[i].distance_to(points[(i + 1) % n])
        var steps: int = clampi(int(ceil(chord / spacing)), 1, 512)
        subdiv[i] = steps
        total += steps

    positions.resize(total)
    tangents.resize(total)
    widths.resize(total)
    distances.resize(total)
    grades.resize(total)
    count = total

    # --- pass 2: evaluate the smoothed curve ------------------------------
    var out: int = 0
    for i: int in n:
        var p0: Vector3 = points[(i - 1 + n) % n]
        var p1: Vector3 = points[i]
        var p2: Vector3 = points[(i + 1) % n]
        var p3: Vector3 = points[(i + 2) % n]
        var w1: float = track_widths[i]
        var w2: float = track_widths[(i + 1) % n]
        var steps: int = subdiv[i]

        for s: int in steps:
            var t: float = float(s) / float(steps)
            positions[out] = _catmull_rom(p0, p1, p2, p3, t)
            widths[out] = lerpf(w1, w2, t)
            out += 1

    _finish()
    return true
```

`clampi(int(ceil(chord / spacing)), 1, 512)` is doing three jobs. `ceil` before
`int` because truncation would give zero samples for a short span. The floor of
1 because a span of zero length must still produce a sample or the arrays
desynchronise. The ceiling of 512 because a corrupt waypoint at (0, 0, 1e30)
would otherwise ask for four billion samples and take the process with it.

### Pass 3: arc length, tangents, grade

```gdscript
func _finish() -> void:
    # Distances are measured on the ground plane so lap progress, AI
    # look-ahead and gate spacing do not stretch on a climb. A 100-unit lap
    # with 8 units of climbing is still a 100-unit lap.
    var acc: float = 0.0
    for i: int in count:
        distances[i] = acc
        var a: Vector3 = positions[i]
        var b: Vector3 = positions[(i + 1) % count]
        acc += Vector2(b.x - a.x, b.z - a.z).length()
    length = acc

    for i: int in count:
        # Central difference: the tangent at i uses its neighbours, not the
        # segment starting at i, so it is smooth across sample boundaries.
        var prev: Vector3 = positions[(i - 1 + count) % count]
        var next: Vector3 = positions[(i + 1) % count]
        var dir: Vector3 = next - prev

        var rise: float = dir.y
        dir.y = 0.0
        var run: float = dir.length()
        if run > 1e-6:
            tangents[i] = dir / run
            grades[i] = rise / run
        else:
            tangents[i] = Vector3.FORWARD
            grades[i] = 0.0

    print("SPLINE: %d samples, loop length %.2f units" % [count, length])
```

Two details worth pausing on.

**Horizontal arc length.** Every distance in the game — gate positions, AI
look-ahead, the "how far ahead is the car in front" readout — is measured on the
ground. If arc length included the vertical, a hilly circuit would report a
longer lap than a flat one of the same map footprint, and the AI's 1.4-unit
look-ahead would shrink on climbs exactly where it needs to be longest.

**`Vector3.FORWARD` is `(0, 0, -1)`** in Godot, which is the -Z convention from
chapter 05. The C version writes `(Vector3){0, 0, 1}` here. This is one of the
four places the π difference shows up, and it is a degenerate fallback that
should never trigger — but if it does, having it point *forward* rather than
backwards means a car on that sample drives the right way round the circuit.

### Building from a `Curve3D` instead

Pipeline B authors the line as a `Path3D`. Then Godot has already done the
interpolation and you only need the resample:

```gdscript
func build_from_curve(curve: Curve3D, width: float, spacing: float = 0.25) -> bool:
    if curve.point_count < 3:
        push_error("SPLINE: curve has %d points, need at least 3" % curve.point_count)
        return false

    curve.bake_interval = spacing
    var baked: float = curve.get_baked_length()
    var total: int = maxi(3, int(round(baked / spacing)))

    positions.resize(total)
    # ... resize the rest ...
    count = total

    for i: int in total:
        # sample_baked walks the curve's own arc-length table, so this is a
        # genuinely uniform resample rather than uniform in the Bezier
        # parameter — which would bunch samples in the corners.
        positions[i] = curve.sample_baked(baked * float(i) / float(total), true)
        widths[i] = width

    _finish()
    return true
```

`sample_baked(offset, cubic = true)` interpolates between baked points cubically;
`false` uses linear, which is measurably faster and, at a 0.25 spacing on a
circuit whose corners have a radius of about 2 units, visually identical. This
runs once per level load, so take the cubic.

---

## The nearest-point query

This is the function the whole chapter exists for. It runs twice per car per
tick — before the physics, to find the surface, and after, to update progress.

### Projecting onto one segment

```gdscript
## Projects `point` onto the segment starting at sample i, returning the
## parameter along it clamped to [0, 1], and writing the squared distance.
func _project(i: int, point: Vector3, out_sq: Array[float]) -> float:
    var j: int = (i + 1) % count
    var a: Vector3 = positions[i]
    var b: Vector3 = positions[j]

    # Everything horizontal: the car is solved on the XZ plane and a 15%
    # gradient would otherwise bias the projection towards the uphill sample.
    var abx: float = b.x - a.x
    var abz: float = b.z - a.z
    var apx: float = point.x - a.x
    var apz: float = point.z - a.z

    var denom: float = abx * abx + abz * abz
    var t: float = 0.0
    if denom > 1e-9:
        t = clampf((apx * abx + apz * abz) / denom, 0.0, 1.0)

    var dx: float = apx - abx * t
    var dz: float = apz - abz * t
    out_sq[0] = dx * dx + dz * dz
    return t
```

`clampf(…, 0, 1)` is what makes this a *segment* projection rather than a line
projection: without it, a car in the middle of a hairpin projects onto the
infinite extension of a segment it is nowhere near, and the query returns a
point off the end of the track.

The `Array[float]` out-parameter is GDScript's answer to C's `float *outSqDist`.
It is not pretty. The alternatives are worse: returning a `Vector2` of
`(t, sq_dist)` reads badly at every call site, and returning a small object
allocates. Chapter 16 replaces this whole function anyway.

### The two-tier search

```gdscript
## Nearest point on the centre line. `hint` is the caller's previous result
## (negative for a full search); the returned SplineQuery carries the new one
## in `index`. Fills `out` in place rather than allocating.
func closest_into(point: Vector3, hint: int, out: SplineQuery) -> void:
    var best: int = 0
    var best_t: float = 0.0
    var best_sq: float = INF
    var sq: Array[float] = _sq_scratch     # one-element scratch, allocated once

    # --- fine sweep around the caller's last answer -----------------------
    # Where a car that moved normally will be: at 6.6 u/s and 120 Hz it
    # travelled 5.5 cm, or roughly a fifth of one sample spacing.
    if hint >= 0:
        var h: int = hint % count
        for k: int in range(-SEARCH_WINDOW, SEARCH_WINDOW + 1):
            var i: int = posmod(h + k, count)
            var t: float = _project(i, point, sq)
            if sq[0] < best_sq:
                best_sq = sq[0]
                best = i
                best_t = t

    # --- coarse sweep of the whole loop -----------------------------------
    # Without this a stale hint — after a respawn, or on a circuit whose two
    # halves run close together — can pin the query to the wrong side of the
    # track, and no distance threshold reliably detects that. The stride keeps
    # it cheap enough to run always.
    var stride: int = maxi(1, count / 64)
    var coarse_best: int = -1
    var coarse_sq: float = best_sq
    var i2: int = 0
    while i2 < count:
        var t2: float = _project(i2, point, sq)
        if sq[0] < coarse_sq:
            coarse_sq = sq[0]
            coarse_best = i2
            best_t = t2
        i2 += stride

    # Refine around the coarse winner when it beats the windowed result.
    if coarse_best >= 0:
        best_sq = coarse_sq
        best = coarse_best
        for k: int in range(-stride, stride + 1):
            var i3: int = posmod(coarse_best + k, count)
            var t3: float = _project(i3, point, sq)
            if sq[0] < best_sq:
                best_sq = sq[0]
                best = i3
                best_t = t3

    _fill(out, point, best, best_t)
```

**The coarse sweep is the interesting half**, and the C series is right to spend
a page on it. The naive design is: search the window, and if the best distance
is suspiciously large, fall back to a full search. That does not work, and the
reason is worth understanding because the same trap appears in every spatial
cache:

> A stale hint does not produce a *large* distance. It produces a plausible
> small one, on the wrong part of the track.

On a circuit whose back straight runs 3 units from the pit straight, a car on
one is 3 units from the other — well inside any threshold you would pick, and
comfortably "on track" if the threshold is generous. The car's lap counter then
runs backwards. The only reliable detector is to look everywhere, so the design
looks everywhere, cheaply: `count / 64` gives 64 probes regardless of circuit
size, and the refinement pass recovers the precision the stride threw away.

Total per query: 49 windowed + 64 coarse + up to 125 refinement ≈ **240 segment
tests**, against `Curve3D.get_closest_offset`'s 4,000. And it returns lateral
offset, width and grade, which `Curve3D` does not.

`posmod(h + k, count)` is Godot's floored modulo — it returns a non-negative
result for negative inputs, so it replaces C's `((h + k) % n + n) % n`. Using
plain `%` here is a real bug: `-3 % 4000` is `-3` in GDScript, and indexing a
packed array with it raises.

### Assembling the result

```gdscript
func _fill(out: SplineQuery, point: Vector3, best: int, t: float) -> void:
    var next: int = (best + 1) % count
    var a: Vector3 = positions[best]
    var b: Vector3 = positions[next]

    out.index = best
    out.position = a.lerp(b, t)               # carries the surface height
    out.tangent = tangents[best]
    out.half_width = lerpf(widths[best], widths[next], t) * 0.5
    out.grade = lerpf(grades[best], grades[next], t)

    # Horizontal, to match how sample distances were accumulated.
    var seg: float = Vector2(b.x - a.x, b.z - a.z).length()
    out.distance = distances[best] + seg * t
    if out.distance >= length:
        out.distance -= length

    # Sign the lateral offset using the track's right vector (tangent x up).
    var right: Vector3 = Vector3(-out.tangent.z, 0.0, out.tangent.x)
    var rel: Vector3 = point - out.position
    out.lateral = rel.x * right.x + rel.z * right.z
```

`out.position` deliberately keeps the interpolated Y. That single value is how
the car knows what height to sit at and the terrain knows where the road is —
the whole elevation system in chapter 08 and chapter 13 rides on it.

The right vector is `tangent × up = (-tz, 0, tx)`. Check it: a tangent of
`(0, 0, -1)` — Godot forward — gives `(1, 0, 0)`, which is +X, which is right
when facing forward. That is the same right-hand rule as `Convention.right`, and
the two must agree or a car will report itself off-track on the wrong side.

---

## Sampling by arc length

The AI asks "where is the line 1.4 units ahead" several times per tick, so this
has to be cheap too.

```gdscript
## Point and attributes at an arc length, wrapping round the loop.
func sample_into(distance: float, out: SplineQuery) -> void:
    if count < 2 or length <= 0.0:
        return
    distance = fposmod(distance, length)

    # Samples are near-uniform, so start from the proportional guess and walk.
    var i: int = int((distance / length) * float(count)) % count
    for _guard: int in count:
        var d0: float = distances[i]
        var j: int = (i + 1) % count
        var d1: float = length if j == 0 else distances[j]
        if distance < d0:
            i = posmod(i - 1, count)
            continue
        if distance > d1:
            i = j
            continue

        var t: float = (distance - d0) / (d1 - d0) if d1 > d0 else 0.0
        out.position = positions[i].lerp(positions[j], t)
        out.tangent = tangents[i]
        out.half_width = lerpf(widths[i], widths[j], t) * 0.5
        out.grade = lerpf(grades[i], grades[j], t)
        out.distance = distance
        return
```

The proportional guess lands within a sample or two because the resample is
near-uniform by construction, so the loop almost always exits on its first
iteration. The `_guard` bound is not an optimisation, it is a **liveness
guarantee**: if the distance table were ever non-monotonic — a bug in `_finish`,
a NaN from coincident waypoints — the walk would oscillate forever and hang the
game inside a physics tick, with no error message. Bounding it converts an
infinite loop into a wrong answer, which is a much better failure.

`fposmod` is the floating-point floored modulo. `fmod(-1.0, 10.0)` is `-1.0`;
`fposmod(-1.0, 10.0)` is `9.0`. Every wrap in this file uses `fposmod` or
`posmod`, and the C version's two-line `if (d < 0) d += length` dance disappears.

---

## Wrapped differences

```gdscript
## Shortest signed difference `a - b` around the loop, in (-length/2, length/2].
func wrap_delta(a: float, b: float) -> float:
    var half: float = length * 0.5
    var d: float = fmod(a - b, length)
    if d > half:
        d -= length
    if d < -half:
        d += length
    return d
```

Eight lines, and the whole lap-counting system rests on them. "Is the car in
front of the gate or behind it" is `wrap_delta(car_distance, gate_distance) > 0`,
and it has to be right when the car is at 119.8 and the gate is at 0.3 on a
120-unit loop — the answer is +0.5, not −119.5.

Note `fmod` here rather than `fposmod`: the sign is *wanted* before the two
half-length corrections. Chapter 10 uses this for gate crossings and for the
"gap to the car ahead" readout.

---

## How the tests pin it down

The property that makes this testable is the one from chapter 04: a level with
nothing but a waypoint ring is a valid level. So a test circuit is six lines and
no file:

```gdscript
# tests/test_spline.gd
## A circle of radius r has a known circumference, a known tangent at every
## point (perpendicular to the radius) and a known nearest point (radially
## outward). Three independent properties from one primitive.
func _ring(radius: float, points: int, width: float) -> LevelData:
    var level := LevelData.new()
    level.waypoint_positions.resize(points)
    level.waypoint_widths.resize(points)
    for i: int in points:
        var a: float = TAU * float(i) / float(points)
        level.waypoint_positions[i] = Vector3(cos(a) * radius, 0.0, sin(a) * radius)
        level.waypoint_widths[i] = width
    return level

func test_loop_length_matches_circumference() -> void:
    var spline := TrackSpline.new()
    assert_bool(spline.build_from_level(_ring(10.0, 64, 0.69), 0.25)).is_true()
    # A 64-gon inscribed in a circle is slightly short; 0.5% is generous.
    assert_float(spline.length).is_equal_approx(TAU * 10.0, TAU * 10.0 * 0.005)

func test_lateral_sign_is_right_of_travel() -> void:
    var spline := TrackSpline.new()
    spline.build_from_level(_ring(10.0, 64, 0.69), 0.25)
    var out := SplineQuery.new()
    # At (10, 0, 0) the ring travels towards +Z, so right = tangent x up = -X,
    # which points at the origin: a car inside the ring is to the *right* of
    # the line here, one unit away.
    spline.closest_into(Vector3(9.0, 0.0, 0.0), -1, out)
    assert_float(out.lateral).is_greater(0.0)
    assert_float(out.lateral).is_equal_approx(1.0, 0.05)

func test_stale_hint_is_recovered() -> void:
    var spline := TrackSpline.new()
    spline.build_from_level(_ring(10.0, 64, 0.69), 0.25)
    var out := SplineQuery.new()
    # A hint from the far side of the ring: the coarse sweep must override it.
    spline.closest_into(Vector3(10.0, 0.0, 0.0), spline.count / 2, out)
    assert_float(out.distance).is_equal_approx(0.0, 0.5)
```

The third test is the one that would have caught the bug the coarse sweep exists
to prevent, and it is the reason to write it explicitly rather than trusting
that a hint is always fresh.

---

## Cost, and what chapter 16 does about it

Per car, per tick: two `closest_into` calls (~240 segment tests each) plus six
`sample_into` calls from the AI. At 120 Hz with six cars that is roughly
**350,000 segment projections a second**, all in GDScript.

That is the single hottest thing in the project, by a wide margin, and it is
where chapter 16 goes first. The design above is what makes that port cheap:
`_project` is 15 lines of pure float arithmetic over `PackedVector3Array`s,
which is a pointer to a flat buffer on the C side. There is no Variant, no
`Object`, no allocation, and no engine call in the inner loop.

**Write the GDScript version first, with tests, then port it.** The tests above
are what tell you the C version agrees.

---

## Exercises

1. **Compare with `Curve3D`.** Build a `Path3D` from `circuit02`'s waypoints and
   time 10,000 `get_closest_offset` calls against 10,000 `closest_into` calls
   with a valid hint. Then repeat with a hint of −1 (full search). Which of the
   three is fastest, and does the ranking change with `bake_interval`?

2. **See the overshoot.** Change `ALPHA` to `0.0` — that is uniform
   Catmull-Rom — and plot the curvature along `circuit02`, or just print the
   minimum radius the AI's probe finds per lap. Find the waypoint span where the
   spacing jumps most and look at what the curve does there.

3. **Break the coarse sweep.** Delete it, keeping only the windowed search. Then
   respawn a car on the opposite side of the circuit (chapter 10's rescue does
   this). What does the lap counter do, and how long does it take to recover?

4. **Cheaper widths.** `widths` is a `PackedFloat32Array` of 4,000 values, and
   almost every circuit uses one constant width throughout. Add a fast path for
   the constant case and measure whether it is worth the branch.

5. **Grade, verified.** Build a ring whose Y follows `sin` of the angle, so the
   loop climbs and descends twice. Assert that `grades` integrates to zero
   around the loop (it must — it is a closed curve) and that its extremes land a
   quarter-loop from the height extremes.

---

Next: [07 — Collision](07-collision.md)
