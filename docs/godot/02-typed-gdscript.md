# 02 — Typed GDScript

> The rule this whole series follows, why it is a rule rather than a preference,
> and how to make the parser enforce it. No C-series equivalent — in C the
> compiler already did this for you.

---

## The problem

GDScript will let you write this:

```gdscript
# Every line here is legal. Three of them are wrong.
var tuning = load("res://scripts/game/car_tuning.gd")
var grip = tuning.grip_tarmac
var decay = exp(-grip * delta)
velocity = velocity * decay
```

`tuning` is a `GDScript`, not a `CarTuning`, so `grip_tarmac` is `null`.
`-grip * delta` is `null * float`, which GDScript evaluates as `0.0` rather than
raising. `decay` is therefore `1.0`, the car never loses lateral velocity, and
you have a racer with infinite grip and no error message anywhere.

You find it forty minutes later by printing things.

This is not a contrived example; it is the characteristic GDScript bug. Dynamic
typing turns a class of mistakes that *should* be caught when the file is parsed
into mistakes that are caught when a specific line runs — and in a game, "when
that line runs" can mean "on lap 3, in the wet, on someone else's machine".

The same code, typed:

```gdscript
var tuning: CarTuning = load("res://levels/tuning/default.tres") as CarTuning
var grip: float = tuning.grip_tarmac
var decay: float = exp(-grip * delta)
velocity *= decay
```

Now `tuning.grip_tarmac` is resolved at parse time against a known class. If
`CarTuning` has no such property, the script fails to compile — in the editor,
in red, before you press play. If the `load` returns the wrong resource type,
`as` gives you `null` and the very next line reports it.

---

## The three ways to declare a variable

GDScript has three, and only two of them are allowed in this project.

```gdscript
var a = 6.6                 # 1. untyped     — BANNED
var b := 6.6                # 2. inferred    — allowed, sparingly
var c: float = 6.6          # 3. explicit    — the default choice
```

**1. Untyped** makes `a` a `Variant`. Every read boxes and unboxes, every method
call is a runtime hash lookup, and the editor can tell you nothing about it.

**2. Inferred** (`:=`) is fully static: `b` is a `float` forever, exactly as if
you had written `: float`. The type is taken from the right-hand side at parse
time. It is not dynamic typing and it costs nothing at runtime.

**3. Explicit** says the type out loud.

This project prefers explicit and uses inference only when the right-hand side
is a constructor call, where the type is already on the line:

```gdscript
var driver := AIDriver.new()                    # obvious, no repetition
var samples: PackedVector3Array = []            # explicit: [] infers Array
var half_width: float = query.half_width        # explicit: reads as documentation
```

That middle line is the reason to be suspicious of inference generally. `var
x := []` infers the *untyped* `Array`, not `Array[float]`, and `var n := 0`
infers `int` when you may well have meant `float` — after which `n = 0.5`
silently truncates or raises a narrowing warning. Inference is only as good as
the literal you fed it, and literals are where you are least likely to be
thinking about types.

### Functions

Every parameter and every return is annotated, including `-> void`:

```gdscript
# scripts/engine/track_spline.gd
func closest(point: Vector3, hint_index: int) -> SplineQuery:

func sample_at(distance: float) -> SplineSample:

func _rebuild_arc_lengths() -> void:
```

`-> void` is not decoration. Without it the function returns `Variant` (well:
`null`, typed as `Variant`), and any caller that uses the result in an
expression gets an unsafe line rather than a compile error.

### Containers

Untyped `Array` is a `Variant` in a trench coat: every element read is a
Variant, so a typed function that consumes them is doing runtime conversions
element by element.

```gdscript
var waypoints: Array[LevelWaypoint] = []          # typed array (4.0+)
var distances: PackedFloat32Array = []            # packed, for numbers
var positions: PackedVector3Array = []            # packed, for vectors
var by_name: Dictionary[String, int] = {}         # typed dictionary (4.4+)
```

Choosing between `Array[float]` and `PackedFloat32Array` matters more than it
looks, and chapter 03 covers the memory side. The short version:

| | Element type | Storage | Best for |
|---|---|---|---|
| `Array[float]` | 64-bit `float`, boxed as Variant internally | pointer array | Small, mixed-lifetime, needs `Array` methods |
| `PackedFloat32Array` | 32-bit float, unboxed | one flat buffer | Thousands of numbers, iterated per tick |
| `PackedVector3Array` | 3 × 32-bit float, unboxed | one flat buffer | Mesh data, spline samples, positions |

The spline's 4,000 arc-length values live in a `PackedFloat32Array`. That is one
allocation of 16 KB rather than 4,000 boxed floats, it is what `ArrayMesh` and
the GDExtension in chapter 16 both want anyway, and — the part people miss —
it is **contiguous**, so walking it is a cache-friendly linear scan in exactly
the way the C engine's arena-allocated arrays were.

If you are on Godot 4.3 or earlier, typed dictionaries do not exist. Do not
reach for an untyped `Dictionary` to hold a record; define a class:

```gdscript
# Not this, on any version:
var racer := { "lap": 0, "next_gate": 0, "score": 0.0 }
# racer.lap is a Variant. racer["laps"] is a typo that returns null.

# This:
class_name RaceProgress extends RefCounted

var lap: int = 0
var next_gate: int = 0
var score: float = 0.0
```

Two lines longer, and now `progress.laps` is a compile error rather than
`null`, `progress.lap` autocompletes, and the whole record is one object rather
than a hash table with string keys. A dictionary is for a mapping whose keys are
data. A record with known fields is a class.

---

## Making the parser enforce it

Discipline that depends on remembering is not discipline. Godot's GDScript
warnings can be promoted to hard errors per warning, in Project Settings under
`debug/gdscript/warnings/`. Turn on `Advanced Settings` to see them.

Set these to **Error** (value `2`; `1` is Warn, `0` is Ignore):

```ini
; project.godot
[debug]

gdscript/warnings/untyped_declaration=2
gdscript/warnings/inferred_declaration=1
gdscript/warnings/unsafe_property_access=2
gdscript/warnings/unsafe_method_access=2
gdscript/warnings/unsafe_call_argument=2
gdscript/warnings/unsafe_cast=2
gdscript/warnings/narrowing_conversion=2
gdscript/warnings/return_value_discarded=1
gdscript/warnings/integer_division=1
gdscript/warnings/shadowed_variable=1
```

What each one actually catches:

**`untyped_declaration`** — the whole rule, in one setting. `var a = 6.6` stops
compiling. This is the single most valuable line in this file.

**`inferred_declaration`** — left at *Warn*, not Error. Inference is safe; this
warning exists so that when you use it, you have decided to. If you would rather
ban it outright, set it to 2 and the project still compiles: nothing here relies
on `:=` that could not be written explicitly.

**`unsafe_property_access` / `unsafe_method_access`** — these are the ones that
catch the class of bug in the opening example. They fire when you read a
property or call a method on something the parser only knows as `Variant`,
`Node` or `Object`. This is what forces you to cast the results of `get_node`,
`load` and `JSON.parse_string` at the boundary instead of letting them travel.

**`unsafe_call_argument`** — passing a `Variant` where a typed parameter is
expected. Catches `spline.sample_at(json_data["distance"])`.

**`unsafe_cast`** — a cast the parser cannot verify. Makes you notice where you
are asserting rather than proving.

**`narrowing_conversion`** — assigning a `float` to an `int` variable. In a
physics codebase this is nearly always a bug: `var steps: int = length / spacing`
silently floors, and you find out when a spline is one sample short at exactly
one track length.

**`integer_division`** — `1 / 2` is `0` in GDScript, as in C. Left at *Warn*
because it is occasionally what you want; every deliberate use in this project
carries a `@warning_ignore("integer_division")` and a comment saying why.

Two more things make this practical day to day:

- Turn on **Editor Settings → Text Editor → Appearance → Gutters → Highlight
  Type Safe Lines.** Line numbers are then tinted for lines the parser has fully
  type-checked. A block of un-tinted lines in the middle of a hot function is a
  visible smell, and the fastest way to find where a `Variant` got in.
- Run `godot --headless --check-only --script res://scripts/game/car_body.gd`
  in CI, or simply `godot --headless --quit` on the project, which parses every
  script. With the warnings above set to Error, a type mistake fails the build.
  Chapter 15 wires this into the test target.

---

## Where typing gets awkward, and what to do

Being honest about this is more useful than pretending it is free. There are
five places where the type system pushes back.

### 1. `get_node` returns `Node`

```gdscript
# Unsafe: the parser knows only that this is a Node.
@onready var car = $Car
car.apply_input(input)          # unsafe_method_access

# Better: assert the type once, at the declaration.
@onready var car: CarBody = $Car as CarBody

# Best: do not look it up at all.
@export var car: CarBody
```

The `as` form yields `null` on a type mismatch rather than raising, so a wrong
scene layout gives you a clean `null` at `_ready` instead of a confusing failure
three subsystems later. Add the check where it matters:

```gdscript
func _ready() -> void:
    assert(car != null, "Car node missing or wrong type")
```

The `@export` form is better still because it removes the string entirely: the
reference is stored in the `.tscn`, the editor validates the type when you drag
the node in, and renaming the node cannot break it. Every cross-node reference
in this project that is not a direct child uses `@export`.

### 2. `load` and `ResourceLoader` return `Resource`

Same shape, same fix:

```gdscript
var level: LevelData = ResourceLoader.load(path) as LevelData
if level == null:
    push_error("MAIN: '%s' is not a LevelData" % path)
    return false
```

`preload` is better where the path is a constant, because it resolves at parse
time and the parser knows the concrete type:

```gdscript
const DEFAULT_TUNING: CarTuning = preload("res://levels/tuning/default.tres")
```

Note that this is also a `const` — chapter 03 explains why that matters for when
the resource is loaded and freed.

### 3. Anything crossing a serialisation boundary is `Variant`

`JSON.parse_string`, `FileAccess.get_var`, `Dictionary` from a network packet,
`Object.get(name)` — all `Variant`, unavoidably. The rule is not to avoid them,
it is to **convert at the boundary and never let a `Variant` travel**:

```gdscript
# scripts/data/level_json.gd
## Reads the C engine's .level.json format into typed data. Every field is
## validated here so nothing downstream ever sees a Variant.
static func waypoints_from_json(raw: Variant) -> Array[LevelWaypoint]:
    var out: Array[LevelWaypoint] = []
    if not (raw is Array):
        push_error("LEVEL: 'waypoints' is not an array")
        return out

    for entry: Variant in raw as Array:
        if not (entry is Dictionary):
            continue
        var dict: Dictionary = entry as Dictionary
        var wp := LevelWaypoint.new()
        wp.position = _vec3(dict.get("pos", null))
        wp.width = float(dict.get("width", 0.69))
        out.append(wp)
    return out
```

One function is dirty so a hundred are clean. This is exactly what the C
engine's `level.c` does with its `JsonFloatField(o, "width", 0.69f)` helpers,
and for the same reason.

### 4. Lambdas and `Callable`

`Callable` carries no signature. `array.map(func(x): return x * 2)` is untyped
on both ends. You can annotate lambda parameters — `func(x: float) -> float:` —
which types the body, but the `Callable` itself is still opaque to the parser.

For hot loops this project uses a plain `for` rather than `map`/`filter`, which
is both typed and faster. For callbacks it prefers signals, whose parameters are
declared:

```gdscript
signal lap_completed(racer_index: int, lap: int, lap_time: float)
```

Those types are checked by the editor when you connect in the inspector and
serve as documentation everywhere else.

### 5. Nullability is not in the type system

`var level: LevelData` can be `null`, and nothing warns you. There is no
`LevelData?` and no non-nullable annotation. The mitigations:

- Initialise at declaration wherever possible.
- `assert()` at the seams. Assertions are stripped from release exports, so they
  cost nothing shipped.
- For freed `Object`s specifically, `is_instance_valid(obj)` — a reference to a
  freed node is not `null`, it is a dangling reference that *reports* as
  non-null and raises on use. `RefCounted` and `Resource` do not have this
  problem, which is one more reason chapter 03 prefers them.

---

## What typing actually buys, measured

Two things: errors move earlier, and the interpreter gets faster. The first is
the reason. The second is real but worth measuring rather than assuming, so here
is a harness rather than a claim:

```gdscript
# tests/bench/bench_typing.gd
## Run: godot --headless --script res://tests/bench/bench_typing.gd
extends SceneTree

const ITERATIONS: int = 2_000_000

func _init() -> void:
    print("typed:   %d ms" % _typed())
    print("untyped: %d ms" % _untyped())
    quit()

func _typed() -> int:
    var start: int = Time.get_ticks_msec()
    var acc: float = 0.0
    for i: int in ITERATIONS:
        acc += sqrt(float(i)) * 0.5
    return Time.get_ticks_msec() - start

@warning_ignore("untyped_declaration")
func _untyped():
    var start = Time.get_ticks_msec()
    var acc = 0.0
    for i in ITERATIONS:
        acc += sqrt(float(i)) * 0.5
    return Time.get_ticks_msec() - start
```

Run it on your target hardware — a desktop and a Pi will not agree on the ratio.
The shape of the result is consistent, though, and worth understanding rather
than memorising:

- **Arithmetic on typed locals** compiles to specialised opcodes that operate on
  raw `float`/`int` slots. The untyped version boxes each intermediate into a
  `Variant` and dispatches the operator through a lookup table. This is where
  most of the difference lives.
- **Method calls on typed objects** resolve to a direct index into the method
  table. Untyped calls do a string hash lookup per call.
- **Typed array element access** skips a Variant conversion per read.
- **Packed arrays** skip it *and* the pointer chase.

What typing does **not** speed up: engine calls (`global_position`,
`move_and_slide`, `Vector3.normalized()`) are the same cost either way, because
they cross into C++ regardless. In a script dominated by engine calls, typing
buys you correctness and roughly nothing else. In a script dominated by
arithmetic — `track_spline.gd`, `ai_driver.gd`, `terrain_builder.gd` — it buys
you a large constant factor, and it is the reason chapter 16 has only three
functions left to move to C rather than fifteen.

---

## House style

The rest of the conventions this project follows, briefly, so later chapters do
not have to explain themselves:

```gdscript
# scripts/game/car_tuning.gd
class_name CarTuning
extends Resource

## Arcade vehicle tuning, in the Kenney kit's scale: one world unit is one road
## tile, the drivable lane is 0.69 wide, a car is 0.30 x 0.60.

@export_group("Engine")
## Forward acceleration at full throttle, u/s^2.
@export var engine_power: float = 7.2
## u/s^2.
@export var brake_power: float = 13.0
@export var top_speed: float = 6.6
@export var reverse_speed: float = 2.0

@export_group("Grip")
## Lateral velocity decay rate, 1/s.
@export_range(0.0, 20.0, 0.1) var grip_tarmac: float = 9.5
@export_range(0.0, 20.0, 0.1) var grip_grass: float = 3.0
@export_range(0.0, 20.0, 0.1) var grip_sand: float = 6.0
## Multiplier applied to grip while the handbrake is down.
@export_range(0.0, 1.0, 0.01) var handbrake_grip: float = 0.22
```

- **`class_name` on anything referenced by type.** It is what makes `CarTuning`
  usable as an annotation from another file, and what puts it in the editor's
  "create node/resource" list.
- **`##` doc comments, not `#`,** on anything exported. They show up in the
  inspector tooltip and in generated documentation. `#` is for notes to the
  reader of the code.
- **`@export` over hard-coded constants** for anything a designer might touch,
  **`const` for anything they must not**. `const RACE_GATE_WINDOW: float = 3.0`
  is a fact about the algorithm; `@export var top_speed` is a decision about
  feel.
- **`@export_range` wherever a value has a sane domain.** It gives you a slider
  and, more usefully, documents the range for the next reader.
- **`_leading_underscore` for private members**, since GDScript has no access
  modifiers. Enforced by convention only.
- **Units in the comment, always.** `1/s`, `u/s^2`, `radians`, `world units`.
  The C series makes this point too and it is the single highest-value comment
  habit in a physics codebase.
- **`snake_case` for functions and variables, `PascalCase` for classes,
  `SCREAMING_SNAKE` for constants and enum members.** Matches the engine's own
  API, so mixed code reads as one thing.

---

## Exercises

1. **Turn the errors on.** Add the `[debug]` block above to `project.godot` in a
   fresh project, paste in any GDScript tutorial code you have lying around, and
   count the errors. Fix them without changing behaviour.

2. **Find the Variant.** Turn on type-safe line highlighting and open
   `ai_driver.gd`. Every un-tinted line is a place a `Variant` exists. There
   should be none — if the project is written as this series describes. Break it
   deliberately: change one `@export var spline: TrackSpline` to
   `@export var spline: Resource` and watch how far the un-tinted region spreads
   from that one declaration.

3. **Measure your hardware.** Run the benchmark above on your development
   machine and on your slowest target. Then add a third case: the same loop but
   with `acc` as a `Variant` and everything else typed. Which of the two changes
   costs more?

4. **The narrowing bug.** Write `var steps: int = 12.0 / 0.25` and predict the
   result before running it. Now set `narrowing_conversion` to Ignore and try
   `var steps: int = spline_length / spacing` with a length of 47.9 and a
   spacing of 0.25. How many samples short is the spline, and where would that
   first show up in the game?

5. **Records, not dictionaries.** Take the `RaceProgress` class from chapter 10
   and rewrite it as a `Dictionary`. Then rewrite `race_director.gd`'s standings
   comparator against it. Count the ways a typo can now produce a wrong result
   rather than an error.

---

Next: [03 — Data and lifetime](03-data-and-lifetime.md)
