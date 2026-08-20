# 03 — Data and lifetime

> What replaces the arena allocator. `Object`, `RefCounted`, `Resource`, `Node`
> — who owns what, when it dies, and how to keep the hot path from allocating.
> Mirrors [C chapter 02](../learn/02-memory-arenas.md).

---

## The problem

Loading a circuit creates a lot of small, related things. `circuit02` has 1,237
props, 300 colliders, 78 sand traps, 489 waypoints and 32 lights, plus a spline
resampled to about 4,000 points and a collision grid with a few thousand cell
references.

In C that was ~1,250 `malloc`s that all had to be freed exactly once, in the
right order, on every early-return path — and chapter 02 of that series solves
it by giving them all one lifetime and one `free`.

In Godot you do not call `malloc` and you cannot arena-allocate. So the question
changes. It is no longer "how do I free this correctly", because reference
counting does that. It becomes two different questions:

1. **Who owns this, and when does the count actually reach zero?** Godot has
   three ownership models and picking the wrong one is how you get a leak, a
   dangling reference, or — the specific Godot classic — a resource that two
   cars are silently *sharing* when you meant them to have one each.
2. **What allocates while the game is running?** 120 physics ticks a second
   through code that allocates is how a GDScript project acquires a stutter that
   profiling attributes to "the garbage collector", except GDScript has no
   garbage collector, so it is really allocator churn and it is entirely yours.

---

## The three families

Everything in Godot descends from `Object`. What matters is which of three
lifetime contracts it signs.

| Base | Freed by | Use for | Danger |
|---|---|---|---|
| `Object` | you, manually, with `free()` | almost nothing | Forget and it leaks; free twice and it crashes |
| `RefCounted` | reference counting | logic objects, records, algorithms | Reference *cycles* never free |
| `Resource` | reference counting | data you save, load or share | The load cache shares instances |
| `Node` | the scene tree, via `queue_free()` | things with a transform or a tick | Freed at end of frame, not now |

### `RefCounted` is the default

If it does not need to be in the tree and does not need to be saved to disk, it
is a `RefCounted`. This is the closest thing Godot has to a C struct with a
known lifetime:

```gdscript
# scripts/engine/collision_grid.gd
class_name CollisionGrid
extends RefCounted

## Uniform grid over a level's static colliders, in compressed-row form:
## cell_start[c]..cell_start[c + 1] indexes into cell_items, which holds
## indices into boxes. Built once per circuit, dropped with it.
##
## This is chapter 07's deterministic collision path — the one you take when
## tests or networking need results the physics server cannot promise. It is
## used here because it is the clearest example in the project of an object
## whose lifetime exactly matches a circuit's.

var boxes: Array[Obb2] = []
var heights: PackedFloat32Array = []
var cell_start: PackedInt32Array = []
var cell_items: PackedInt32Array = []
var grid_w: int = 0
var grid_h: int = 0
var cell_size: float = 1.0
var origin: Vector2 = Vector2.ZERO
```

Whoever built it holds the only reference. When that reference is dropped on a
circuit change, the count hits zero, and every packed array inside it is
released in the same instant. That is the arena's guarantee — one owner, one lifetime, one drop
— arrived at from the other direction.

**No `_ready`, no `_process`, no tree.** A test constructs one in a line:

```gdscript
var grid := CollisionGrid.new()
grid.build(level.colliders, 1.0)
```

which is exactly as cheap as the C version's `CollisionWorldBuild`, and needs no
window, no scene and no `SceneTree`.

### `Resource` is `RefCounted` plus a filename — and a cache

`Resource` adds three things: it can be saved to `.tres`/`.res`, it appears in
the inspector, and **`load()` caches it by path**.

That last one is the sharpest edge in Godot, so it deserves the treatment the C
series gave to alignment:

```gdscript
var a: CarTuning = load("res://levels/tuning/default.tres")
var b: CarTuning = load("res://levels/tuning/default.tres")
a.grip_tarmac = 2.0
print(b.grip_tarmac)     # 2.0  — a and b are the same object
```

This is not a bug, it is the point: a level referencing the same tree model 400
times should hold one mesh, not 400. But it means **a `Resource` is shared
state by default**, and if you hand every car the same `CarTuning` and then let
one car modify it, you have modified all of them. In the editor you have also
modified the file on disk the next time it is saved.

Three rules keep this straight, and this project follows all three:

**1. Resources are read-only at runtime unless you say otherwise.** `CarTuning`,
`LevelData` and `TrackSpline` are written once at load and never mutated during
a race. Anything mutable per-car — the car's own velocity, its progress, its AI
state — lives in a `RefCounted` or on the node, never on a shared resource.

**2. When you do need a private copy, take one explicitly.**

```gdscript
# Deep copy: nested resources are duplicated too. Without `true`, the copy
# shares the originals' sub-resources, which is usually not what you meant.
var tuning: CarTuning = DEFAULT_TUNING.duplicate(true) as CarTuning
tuning.top_speed *= 0.94        # this car is the slow one
```

**3. `@export var resource_local_to_scene: bool` exists for the same problem.**
Setting a resource's *Local to Scene* flag in the inspector makes every
instantiation of the containing scene get its own copy automatically. Use it
when the sharing is per-instance by nature — a `ShaderMaterial` whose colour is
per-car, for example. This project uses it for exactly that: the car body
material, so eight racers can be eight colours without eight `.tres` files.

There is also `ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE)`
when you want a genuinely fresh instance and no cache entry. It is the right
call in tests, and almost never the right call in a game.

### `Node` is tree-owned, and dies late

Nodes are freed by the tree, and the thing to internalise is that `queue_free()`
means *at the end of this frame*, not now:

```gdscript
old_stage.queue_free()
# old_stage is STILL in the tree here.
# It will still receive _physics_process this tick.
# get_first_node_in_group("racers") will still find its cars.
# is_instance_valid(old_stage) is still true.
```

Chapter 01's `_free_stage` calls `remove_child()` first for this reason: removing
it from the tree makes it inert immediately, and the `queue_free` then only has
to reclaim the memory.

The other half of the contract: **a freed `Node` reference is not `null`.** It is
a dangling reference that compares non-null and raises on use. Hence:

```gdscript
if is_instance_valid(target) and not target.is_queued_for_deletion():
    target.apply_input(input)
```

`RefCounted` and `Resource` have neither problem: they cannot be freed while you
hold a reference. That is a real argument for keeping logic objects out of the
tree, and it is why `AIDriver` is a `RefCounted` rather than a child node of the
car.

---

## The cycle, which is the one real leak

Reference counting has exactly one failure mode, and Godot does not paper over
it: **a cycle never reaches zero.**

```gdscript
# scripts/game/ai_driver.gd     — LEAKS
class_name AIDriver
extends RefCounted

var car: CarBody              # the car holds the driver, the driver holds the car
```

If `CarBody` holds `var ai: AIDriver` and `AIDriver` holds `var car: CarBody`,
neither is ever freed. There is no collector to notice. Godot will tell you, but
only if you ask: run with `--verbose` and look for the leaked-object report at
exit, or call `print_orphan_nodes()`.

Three ways out, in order of preference:

**Pass it in, do not store it.** The AI needs the car for the duration of one
call, so take it as a parameter. This is what the C version does — `AIThink`
receives `const Car *car` — and it is why the C version cannot have this bug:

```gdscript
# scripts/game/ai_driver.gd
func think(car: CarBody, tuning: CarTuning, spline: TrackSpline,
           neighbours: Array[AINeighbour], delta: float) -> CarInput:
```

**Own in one direction only.** The race director owns the drivers *and* the
cars; neither owns the other. The ownership graph is a tree, so it cannot
cycle.

**`WeakRef` when a back-reference is genuinely needed**:

```gdscript
var _owner_ref: WeakRef = null

func set_owner_car(car: CarBody) -> void:
    _owner_ref = weakref(car)

func _owner_car() -> CarBody:
    return _owner_ref.get_ref() as CarBody     # null once the car is gone
```

Signals are worth a note here: `connect()` does **not** keep the receiver alive
by default, and a connection to a freed object is cleaned up automatically. But
`connect(callable_that_captures_self)` — a lambda closing over `self` — *does*
create a strong reference, and that is a cycle. If you find a `RefCounted` that
will not die, look for a lambda first.

---

## What replaces the two-pass allocation pattern

The C loader's signature move is: *measure, allocate once, fill*. `SplineBuild`
counts subdivisions and then allocates; `CollisionWorldBuild` counts per-cell
references and then allocates; `EstimateLevelBytes` walks the parsed tree before
touching the arena.

The same pattern is the correct one in GDScript, for the same reason, with
different syntax:

```gdscript
# scripts/engine/track_spline.gd
## Pass 1: subdivide each waypoint span enough that segments stay near
## `spacing`, and count the samples that produces.
var subdiv: PackedInt32Array = []
subdiv.resize(n)
var total: int = 0
for i: int in n:
    var a: Vector3 = waypoints[i].position
    var b: Vector3 = waypoints[(i + 1) % n].position
    var steps: int = clampi(int(ceil(a.distance_to(b) / spacing)), 1, 512)
    subdiv[i] = steps
    total += steps

## Pass 2: one allocation each, then fill by index. Never `append` in a loop
## when the count is known: append grows the buffer geometrically, so a
## 4,000-sample spline reallocates and copies about a dozen times.
_positions.resize(total)
_tangents.resize(total)
_widths.resize(total)
_distances.resize(total)
_grades.resize(total)
```

`resize()` on a packed array is one allocation of exactly the right size, and
the elements come back zeroed — which is the same guarantee `ArenaInit`'s
`calloc` gives, and it is relied on the same way: the fill loop only has to
write the fields it computes.

The difference from `append` is not academic. `append` in a loop over 4,000
samples does roughly a dozen reallocation-and-copy cycles, and each one copies
everything written so far. Building five parallel arrays that way is the
difference between a level load you notice and one you do not.

### Parallel arrays, not an array of objects

Note the shape above: five `Packed*Array`s rather than one
`Array[SplineSample]`. That is deliberate, and it is the same decision the C
engine makes when it stores `SplineSample` structs contiguously in an arena.

```gdscript
# 4,000 samples, five fields each.

# A: Array[SplineSample]  — 4,000 RefCounted objects, each separately
#    allocated, each read a pointer chase. ~200 KB and poor locality.
var samples: Array[SplineSample] = []

# B: parallel packed arrays — five flat buffers, ~112 KB total, linear scan.
var _positions: PackedVector3Array = []
var _tangents: PackedVector3Array = []
var _widths: PackedFloat32Array = []
var _distances: PackedFloat32Array = []
var _grades: PackedFloat32Array = []
```

B wins on memory, on cache behaviour, and — the reason that matters most later —
on being directly passable to a GDExtension in chapter 16, where a
`PackedVector3Array` is a pointer to a flat buffer of floats and needs no
conversion at all.

The cost of B is ergonomics: `SplineQuery` has to be assembled by hand rather
than returned as an element. The project pays it in exactly one place — the
spline — and nowhere else. **Structure-of-arrays is an optimisation, not a
style.** Chapter 06 shows the query API that hides it.

### `Array` is shared; `Packed*Array` is copy-on-write

A subtlety with real consequences:

```gdscript
var a: Array[float] = [1.0, 2.0]
var b: Array[float] = a
b.append(3.0)
print(a.size())          # 3 — Array is a reference type

var c: PackedFloat32Array = [1.0, 2.0]
var d: PackedFloat32Array = c
d.append(3.0)
print(c.size())          # 2 — Packed arrays copy on write
```

So passing a `PackedVector3Array` to a function is free until the callee writes
to it, at which point it silently copies the whole buffer. In a per-tick
function that is a hidden allocation of the exact kind this chapter is about.
Two habits avoid it: functions that read take packed arrays and never write, and
functions that build take an out-parameter they own, or return a fresh one.

---

## Not allocating during a race

The physics tick runs 120 times a second, for every car. Anything it allocates,
it allocates ~1,000 times a second.

**What does not allocate:**

`Vector2`, `Vector3`, `Transform3D`, `Basis`, `Color`, `Rect2`, `Plane` and the
numeric types are value types living in the Variant's inline storage. Arithmetic
on them is free. `car.velocity * delta` allocates nothing. This covers the
overwhelming majority of the vehicle model, which is why chapter 08 reads almost
like the C.

**What does allocate:**

| Pattern | Fix |
|---|---|
| `var list: Array = []` inside the tick | Preallocate once as a member, `clear()` per tick |
| `"lap %d" % lap` | Only in `_process`, and only when the value changed |
| `SomeClass.new()` per tick | Reuse an instance, or make the function `static` |
| `array.append()` in a loop | `resize()` then index-assign |
| `array.map()` / `filter()` | A plain `for` — also typed, also faster |
| `duplicate()` | Never in the tick |
| A `Dictionary` as a return value | A small `RefCounted` record, allocated once |

The neighbour list the AI needs each tick is the case where this actually bites,
because it is rebuilt for every car, every tick — 6 cars × 120 Hz = 720
rebuilds a second. So it is built once and reused:

```gdscript
# scripts/game/race_director.gd
## Scratch buffer for the AI's neighbour list. Allocated once at setup and
## refilled in place: rebuilding a fresh Array here would be ~720 allocations
## a second for a six-car field at 120 Hz.
var _neighbours: Array[AINeighbour] = []

func _ready() -> void:
    _neighbours.resize(RACE_MAX_RACERS)
    for i: int in RACE_MAX_RACERS:
        _neighbours[i] = AINeighbour.new()

func _fill_neighbours(exclude: int) -> int:
    var n: int = 0
    for j: int in _racers.size():
        if j == exclude:
            continue
        var other: CarBody = _racers[j].car
        _neighbours[n].position = other.plane_position
        _neighbours[n].velocity = other.plane_velocity
        n += 1
    return n
```

Note it returns a count rather than resizing the array — the same trick
`CollisionWorldQuery` uses in C, and for the same reason: the buffer's *size* is
its capacity, and the meaningful length is a separate number.

### The best allocator is still none

The C series makes this point with the skid trail ring buffer, and it survives
the port completely intact:

```gdscript
# scripts/game/skid_trails.gd
class_name SkidTrails
extends MultiMeshInstance3D

const MAX_QUADS: int = 512

## Fixed-capacity ring. Oldest marks are overwritten, which is exactly the
## "oldest marks fade first" behaviour we wanted anyway, and it never
## allocates after _ready().
var _next: int = 0

func _ready() -> void:
    multimesh = MultiMesh.new()
    multimesh.transform_format = MultiMesh.TRANSFORM_3D
    multimesh.use_colors = true
    multimesh.instance_count = MAX_QUADS
    multimesh.visible_instance_count = 0

func emit_quad(at: Transform3D, alpha: float) -> void:
    multimesh.set_instance_transform(_next, at)
    multimesh.set_instance_color(_next, Color(0.05, 0.05, 0.06, alpha))
    _next = (_next + 1) % MAX_QUADS
    if multimesh.visible_instance_count < MAX_QUADS:
        multimesh.visible_instance_count += 1
```

When the maximum count is small and known, a fixed buffer beats every dynamic
scheme. `instance_count` is set once; after that, laying rubber is two writes
into a buffer the rendering server already owns.

**The lesson is not "avoid all allocation".** Level loading allocates freely and
should — it happens once and clarity is worth more there. The rule is the same
one the C series lands on, restated for a language with no `free`:

> Match the ownership model to the lifetime pattern. Coincident lifetimes and
> one owner → `RefCounted`. Shared, saved, editable → `Resource`. Has a
> transform or a tick → `Node`. Bounded count → a fixed buffer. Per-tick →
> nothing new at all.

---

## Diagnostics

The arena had `used`, `peak` and a log line. Godot's equivalents are better, and
almost nobody turns them on.

**In the editor:** Debugger → Monitors. Watch `Object Count`, `Resource Count`,
`Node Count` and `Memory/Static` while you load and unload a circuit five times.
Flat lines mean your teardown is complete. A staircase means it is not, and the
step height tells you roughly what is leaking.

**In code**, which is what a test can assert on:

```gdscript
# tests/test_lifetime.gd
## Loading and unloading a circuit must leave nothing behind. This is the
## Godot equivalent of the C engine's "arena.used returns to zero", and it
## catches exactly the same class of bug: a reference held somewhere it
## should not be.
func test_circuit_swap_leaks_nothing() -> void:
    var before: int = int(Performance.get_monitor(Performance.OBJECT_COUNT))

    for i: int in 5:
        main.load_circuit("res://levels/circuit01.tres")
        main.load_circuit("res://levels/circuit02.tres")
    main.free_stage()

    # queue_free() lands at the end of the frame, so let one pass.
    await get_tree().process_frame
    await get_tree().process_frame

    var after: int = int(Performance.get_monitor(Performance.OBJECT_COUNT))
    assert_that(after - before).is_less_equal(8)   # slack for editor internals
```

Note the two `await`s. A test that checks object counts without letting the
deferred deletion queue drain will fail intermittently, and that is a genuinely
horrible bug to chase — it is *your test* that is wrong, not the code.

**At exit:** running the game with `--verbose` prints leaked `Object`s and
their classes on shutdown. A clean run reports none. Make that part of the CI
job in chapter 15 and cycles cannot survive a merge.

---

## Where the arena still wins

Honesty, since this chapter is the one that replaces it.

The C arena has one property Godot has no answer for: **allocation failure is a
testable, deterministic code path.** `tests/test_json.c` exhausts a 64-byte
arena in two lines and asserts the parser fails cleanly. In Godot, running out
of memory is a crash you cannot arrange, so the entire class of "does this
degrade gracefully under memory pressure" test does not exist.

It also has the flat cost model: one `calloc`, no refcount traffic, no cache
lookups, no cycles possible. Reference counting is very cheap, but it is not
free, and it is not *nothing* in a loop that touches a few thousand objects.

What you get in exchange is that the entire category of use-after-free and
double-free is gone, along with the code that prevented it. `LevelUnload` was
one line in C only because chapter 02 did a lot of work; here it is one line
because the language does. That is a good trade for a game, and a bad one for a
console kernel — which is the same conclusion the C series reaches from the
other side.

---

## Exercises

1. **Watch the cache bite.** Give two cars the same `CarTuning` resource without
   duplicating it. In one car's `_ready`, set `tuning.top_speed = 3.0`. Run.
   Explain what happens to the other car, and then explain what happens to the
   `.tres` file on disk if you do the same thing from a `@tool` script.

2. **Make a cycle.** Add `var car: CarBody` to `AIDriver` and assign it. Run the
   game with `--verbose`, quit, and read the leak report. Now fix it with
   `weakref` and read it again. How many objects were leaking per race?

3. **Count the reallocations.** Build the spline with `append` instead of
   `resize`, and time `TrackSpline.build()` on `circuit02` (489 waypoints,
   ~4,000 samples) both ways with `Time.get_ticks_usec()`. Is the difference
   what you expected? Which of the five arrays dominates?

4. **Find the per-tick allocation.** Add
   `print(Performance.get_monitor(Performance.MEMORY_STATIC))` to
   `race_director.gd`'s `_physics_process` and watch it during a race. It should
   be flat. Now build the neighbour list with a fresh `Array` each tick and
   watch it again.

5. **Structure of arrays, both ways.** Implement `TrackSpline` with a single
   `Array[SplineSample]` of `RefCounted` records instead of parallel packed
   arrays. Measure build time, memory (`Performance.MEMORY_STATIC`) and the cost
   of 10,000 `closest()` queries. Then read chapter 16 and explain which of the
   two you would have to undo to move the query into C.

---

Next: [04 — Level data without a parser](04-level-format.md)
