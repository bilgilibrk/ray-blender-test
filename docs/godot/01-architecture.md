# 01 — Architecture

> How a Godot project is split, what may depend on what, and what happens in one
> frame. Mirrors [C chapter 01](../learn/01-architecture.md).

---

## The problem

Unchanged from the C version: a game is a pile of concerns that all want to
touch each other. Physics wants the level. Rendering wants physics. Audio wants
the engine note, which is a fact about a car. The AI wants the track, the other
cars and its own vehicle.

Godot changes *how* that tangle forms, not whether it forms. In C the failure
mode is a single translation unit that includes everything. In Godot it is this:

```gdscript
# The tangle, Godot edition.
var speed: float = get_node("../../Race/Cars/Car0").car.velocity.length()
$"../../UI/HUD".update_speed(speed)
```

Every one of those strings is a dependency on the *shape of the scene tree*,
checked at no point before it runs. Move a node, rename a node, instance the
scene under a different parent, and the game breaks in a way no tool can find
for you. Worse — and this is the same specific failure as in C — **you can no
longer test anything**, because instantiating one system now requires the whole
tree above it to exist.

## The idea: one hard boundary, drawn where the frame lives

The C engine draws exactly one line, hard:

```
game/     knows about racing. Knows nothing about windows, GL, or files.
engine/   knows about windows, GL and files. Knows nothing about racing.
```

The Godot project draws the same line in a different place, because Godot has
already taken the bottom half:

```
scripts/game/     knows about racing. Never touches a Camera3D, a Viewport,
                  a MeshInstance3D, a CanvasItem or an AudioStreamPlayer.
scripts/engine/   knows about meshes, audio streams and input. Never mentions
                  a lap, a checkpoint, a racer or a tuning value.
```

And one softer rule inside it:

> Nothing in `scripts/game/` reads a node it does not own.

"Owns" means: it is a child of this node, declared with `@onready` and a type,
or it was handed in through an exported property or a function argument.
No `get_parent()`, no `../`, no `get_tree().get_first_node_in_group("hud")` from
inside the simulation.

That single restriction is what makes chapter 15's headless race possible: a
`RaceDirector` with six `CarBody` children, running under `godot --headless`,
simulating a complete race with no camera, no HUD and no window.

**Godot makes this much easier than C did**, and it is worth being precise about
why. `--headless` does not disable the scene tree, the physics server, timers,
signals or resource loading — only the rendering driver, which becomes a dummy.
So "headless" in Godot means "the real game, minus pixels". In C it meant "a
separate binary that carefully links everything except the renderer". You get
the same guarantee for far less work; what you have to supply is the discipline
that nothing in the simulation *reaches upwards* for something only the visual
tree provides.

---

## Dependency direction

Arrows point "depends on". There are no cycles, and nothing points up.

```
                     ┌──────────────────┐
                     │  Main (main.tscn)│   wiring, level swap, pause
                     └────────┬─────────┘
        ┌─────────────┬───────┴───────┬──────────────┐
        ▼             ▼               ▼              ▼
  ┌───────────┐ ┌──────────┐  ┌─────────────┐ ┌──────────────┐
  │RaceDirect.│ │   Hud    │  │ SkidTrails  │ │ ChaseCamera  │
  └─────┬─────┘ └──────────┘  └─────────────┘ └──────────────┘
   ┌────┴─────┬──────────────┐
   ▼          ▼              ▼
┌────────┐ ┌──────────┐ ┌──────────────┐
│CarBody │ │ AIDriver │ │ Walls (static)│
└───┬────┘ └────┬─────┘ └──────┬───────┘
    │           │              │
    └───────────┴──────┬───────┘
                       ▼
               ┌────────────────┐
               │  TrackSpline   │
               └───────┬────────┘
                       ▼
               ┌────────────────┐
               │   LevelData    │   (a Resource, loaded from .tres)
               └────────────────┘
```

Read it bottom-up and you get the boot order: a `LevelData` resource backs the
spline, the spline backs the cars, the AI and the collision grid, and the race
director backs everything the player sees.

`PropBatcher`, `TerrainBuilder`, `ProceduralAudio` and the input map hang off the
side. `Main` is the only script that knows they all exist — the same role
`game/src/main.c` plays.

### What each layer may reference

| Layer | May reference | Must not reference |
|---|---|---|
| `data/level_data.gd` | `Resource`, `Vector3`, `Color` | any node type |
| `engine/track_spline.gd` | `LevelData`, maths | any node type, any `game/` script |
| `engine/collision_builder.gd` | `LevelData`, `StaticBody3D` | any `game/` script |
| `game/car_tuning.gd` | `Resource` | everything else |
| `game/car_body.gd` | `CharacterBody3D`, `CarTuning` | `Camera3D`, `Hud`, `RaceDirector` |
| `game/race_director.gd` | `CarBody`, `AIDriver`, `TrackSpline`, `LevelData` | `Camera3D`, `Hud`, `MeshInstance3D` |
| `scenes/main.tscn` + `main.gd` | everything | — |

Notice what is *absent*: `race_director.gd` never mentions the HUD. The race
does not know it is being displayed. Instead it emits signals:

```gdscript
# scripts/game/race_director.gd
signal lap_completed(racer_index: int, lap: int, lap_time: float)
signal race_state_changed(state: RaceState)
signal racer_finished(racer_index: int, position: int, total_time: float)
```

and `main.gd` wires them to the HUD. That single omission is what buys the
headless test — exactly as `race.h` not including `render.h` did in C.

**Signals are the Godot-native way to point downwards.** A signal is a
dependency the *listener* declares, so the emitter stays ignorant. That is the
whole trick: `RaceDirector` announcing "lap 2 completed" is a fact about racing;
whether anything is listening is not its problem.

---

## The frame loop

In C the loop is 60 lines you wrote yourself and can read. In Godot it is inside
the engine, and you attach to it at two points. It is worth knowing the order
exactly, because a surprising number of bugs are "I did this in the wrong one":

```
per frame:
  1. flush input events          -> _input(), _unhandled_input()
  2. physics ticks, 0..N times   -> _physics_process(delta)   delta is FIXED
       - each tick: your code, then the physics server solves and moves bodies
  3. one idle tick               -> _process(delta)           delta VARIES
  4. render
```

Step 2 runs however many times are needed to catch the accumulator up, which is
precisely `FixedStepperAdvance` from the C engine — Godot has already written
it, and it is not configurable beyond two settings:

```gdscript
# scripts/main.gd, in _ready()
Engine.physics_ticks_per_second = 120     # the C build's PHYSICS_HZ
Engine.max_physics_steps_per_frame = 8    # the C build's maxStepsPerFrame
```

Both can also be set in Project Settings under `physics/common/`. The defaults
are 60 and 8. This project uses 120 for the same reason the C one does: at 60 Hz
a car at 6.6 u/s moves 11 cm per tick, which is a third of its own length, and
the wall-contact response gets visibly chunky when you scrape a barrier.

**What Godot has taken off you**, compared to chapter 01 of the C series:

- The accumulator itself.
- The `dt > 0.25` clamp against the spiral of death — Godot clamps internally.
- The catch-up cap. `max_physics_steps_per_frame` is the same idea: if the
  machine genuinely cannot keep up, the simulation runs in slow motion rather
  than freezing. (Godot logs *"Physics ticks per second exceeded"* style
  warnings when this bites — worth watching for on a Pi.)

**What it has not taken off you**: knowing which of the two functions a given
piece of code belongs in. The rule is short:

> If it changes the simulation, it goes in `_physics_process`. If it only
> changes what you see or hear, it goes in `_process`.

| Code | Where | Why |
|---|---|---|
| `CarBody` integration | `_physics_process` | It *is* the simulation |
| `AIDriver.think()` | `_physics_process` | Called from the race director's tick |
| Lap counting, gates | `_physics_process` | Must see every tick, not every frame |
| Chase camera | `_process` | Smoothing; wants the display rate |
| Skid trail spawning | `_process` | Cosmetic, tolerant of jitter |
| HUD updates | `_process` | Nothing reads it at 120 Hz |
| Engine audio parameters | `_process` | The mixer is not tick-locked |

Getting this wrong is rarely a crash. It is worse: it is a simulation whose
results depend on frame rate, which is exactly the bug the fixed step exists to
prevent. A car whose steering is applied in `_process` has different lap times
on different machines.

### Interpolation, the one new problem

A fixed 120 Hz tick and a 60 fps display are fine. A fixed 60 Hz tick and a
144 Hz display is not: two out of three frames show a car that has not moved,
which reads as a subtle stutter.

Godot 4.3 added a global fix — Project Settings →
`physics/common/physics_interpolation` — which makes the engine store two
transforms per body and blend between them for display. Turn it on and the
problem goes away for anything driven by the physics server.

If you are on an earlier version, or you are moving something manually, do it
yourself with the fraction Godot exposes:

```gdscript
# scripts/game/car_visual.gd
# Draws the car mesh between the last two simulated transforms so a 60 Hz
# simulation does not stutter on a 144 Hz display.
extends Node3D

@export var body: CarBody

var _previous: Transform3D = Transform3D.IDENTITY
var _current: Transform3D = Transform3D.IDENTITY

func _physics_process(_delta: float) -> void:
    _previous = _current
    _current = body.global_transform

func _process(_delta: float) -> void:
    var t: float = Engine.get_physics_interpolation_fraction()
    global_transform = _previous.interpolate_with(_current, t)
```

`interpolate_with` slerps the basis and lerps the origin, so a rotating car does
not shear on the way. Note the mesh is a *sibling-level* node driven by the
body, not a child of it: a child would inherit the un-interpolated transform.

### Frame-rate-independent smoothing

The one piece of C-series maths that survives completely intact:

```gdscript
# scripts/engine/smoothing.gd
class_name Smoothing

## Fraction to move towards a target this frame for an exponential approach
## with a time constant of 1/rate seconds. Frame-rate independent: the naive
## `x += (target - x) * 0.1` converges at a speed that depends entirely on how
## often you call it, so the camera is snappy at 144 fps and sluggish at 30.
static func factor(rate: float, delta: float) -> float:
    return 1.0 - exp(-rate * delta)
```

`rate` has units of 1/second; the value settles to within 1/e (about 37%) of its
error after `1/rate` seconds. Every smoothed value in the project — the camera
focus, the camera yaw, the AI's lateral offset, the car's visual pitch — uses
this and nothing else.

Godot's own `lerp(a, b, delta * k)` idiom, which you will see all over sample
code, is the buggy version. `lerp_angle` and `move_toward` have the same
problem. `move_toward` is fine when you *want* a rate limit rather than an ease
— chapter 08 uses it deliberately for exactly that reason.

---

## Nodes, scenes and what actually goes in the tree

The single biggest design decision in a Godot project is what becomes a node and
what stays a plain object. Godot's documentation encourages "everything is a
node"; that is good advice for a first project and wrong for this one.

A `Node` costs you: an entry in the scene tree, `_process` / `_physics_process`
dispatch if you override them, notification traffic, and — the part that
matters here — an object whose lifetime is managed by the tree rather than by
you. In exchange you get: editor visibility, signals, groups, and free
serialisation into a `.tscn`.

So the rule this project uses:

> A node is for something the editor should be able to see, place or configure.
> Everything else is a `RefCounted` or a `Resource`.

| Thing | Type | Why |
|---|---|---|
| A car | `CharacterBody3D` | Has a transform, a collision shape, is placed |
| Its handling numbers | `CarTuning extends Resource` | Data. Editable, saveable, shareable, swappable |
| Its AI | `AIDriver extends RefCounted` | Pure logic over a car. Nothing to see or place |
| The race | `RaceDirector extends Node` | Needs `_physics_process` and the tree's lifetime |
| The centre line | `TrackSpline extends Resource` | Immutable derived data, cached per level |
| The barriers | `StaticBody3D` with many shapes | Placed geometry the physics server owns |
| A level | `LevelData extends Resource` | The whole circuit as data (chapter 04) |
| The HUD | `Control` | Obviously a node |

`AIDriver` being a plain `RefCounted` rather than a node is the one worth
defending, since the tutorial instinct is to make it a child node of the car. As
`RefCounted` it: needs no tree to exist, can be constructed in a test in one
line, has no `_physics_process` of its own that could run in the wrong order
relative to the car's, and disappears the moment the last reference does. It is
called explicitly, from a known point in the tick, exactly like `AIThink` in C.

Chapter 03 goes into the lifetime rules for all three types.

---

## Global state, and where it is allowed

Godot's autoload (singleton) mechanism makes global state so easy that most
projects end up with a dozen. This one has two, and the rule for admitting a
third is the same as the C series' rule for a file-scope `static`:

> A singleton is defensible when the thing it wraps is physically singular. It
> is indefensible when it is merely convenient.

| Autoload | Wraps | Why it is singular |
|---|---|---|
| `Audio` | the mixer and the procedural voice | There is one audio output device |
| `Settings` | resolution, volume, day/night, controls | There is one user |

That is it. Notably **not** autoloads:

- **The race.** You can have two. Chapter 15's tests run dozens of separate
  single-car races in one process to drop cars into gravel traps, which an
  autoload would make impossible.
- **The current level.** It is a `Resource` held by `Main`. Two levels can be
  loaded at once, which is what makes a crossfade between circuits possible.
- **The player.** There is one *now*. Split screen (exercise 3.2) needs two, and
  a `Player` autoload is the single most expensive thing to unpick later.

Godot does not have raylib's excuse here. In C, `render.c` keeps a static struct
because there is exactly one GL context and threading it through every call
would add noise without adding capability. In Godot that struct is
`RenderingServer`, which is already a singleton, already global, and already
written. You do not need your own.

---

## Swapping circuits: the `Stage` pattern, Godot edition

The C version groups every per-level object into one `Stage` struct so it can be
torn down and rebuilt wholesale, and makes every destructor tolerate a zeroed
struct so a half-built stage can still be freed.

Godot's equivalent is a single node you own, free, and rebuild:

```gdscript
# scripts/main.gd
## What survives a change of circuit — the camera, the skid marks, the audio,
## the day/night toggle, the pause state — deliberately lives outside this.
@onready var _stage_root: Node3D = $Stage

var _level: LevelData = null
var _spline: TrackSpline = null
var _walls: StaticBody3D = null
var _race: RaceDirector = null

func load_circuit(path: String) -> bool:
    _free_stage()

    var level: LevelData = ResourceLoader.load(path) as LevelData
    if level == null:
        push_error("MAIN: could not load level '%s'" % path)
        return false

    var spline := TrackSpline.new()
    if not spline.build(level, 0.25):
        push_error("MAIN: level '%s' has too few waypoints" % level.display_name)
        return false

    _level = level
    _spline = spline

    var stage: Node3D = STAGE_SCENE.instantiate()
    _stage_root.add_child(stage)
    # One StaticBody3D holding every barrier as a child shape — chapter 07.
    _walls = CollisionBuilder.build_walls(_level, stage)
    _race = stage.get_node("RaceDirector") as RaceDirector
    _race.setup(_level, _spline, _walls, 6)
    return true

func _free_stage() -> void:
    for child in _stage_root.get_children():
        _stage_root.remove_child(child)
        child.queue_free()
    _race = null
    _walls = null
    _spline = null
    _level = null
```

Three things carry over from the C version, and one is new.

**Carried over — teardown tolerates a half-built stage.** `_free_stage` works
whether `load_circuit` got as far as adding the stage or bailed at the first
`null` check, because iterating an empty child list and assigning `null` over
`null` are both fine. The C version pays one `memset` for this property; the
Godot version pays nothing.

**Carried over — one owner per level-scoped object.** `_level` and `_spline`
are references held by exactly one script. When `main.gd` drops them, they are
gone: `TrackSpline` is a `RefCounted`, so the last reference frees it. `_walls`
is a node, so it dies with the stage that parents it. This is the arena's lifetime guarantee, delivered by
reference counting instead. Chapter 03 covers where that guarantee has holes.

**New — `remove_child` before `queue_free`.** `queue_free` deletes the node *at
the end of the current frame*, not now. Until then it is still in the tree, still
in groups, still receiving `_physics_process`, and still findable by
`get_first_node_in_group`. A stage that is being replaced but has not been
collected yet will happily run one more physics tick against a level that
`main.gd` has already dropped. Removing it from the tree first makes it inert
immediately; the `queue_free` then only reclaims memory.

If you take one Godot-specific habit from this chapter, take that one. "Freed
but still ticking" is the single most common source of `Attempt to call function
on a previously freed instance` in real projects, and it always looks like a
race condition when it is really just deferred deletion.

---

## Why Godot, and what it costs

Neither choice is neutral, so be explicit — same as the C series was about
raylib.

**What you get, measured against what `docs/learn` had to write by hand:**

| C-series chapter | In Godot |
|---|---|
| 02 Arena allocation | Gone. Reference counting and the tree own lifetimes |
| 03 JSON parser | Gone. `Resource` serialisation, or the built-in `JSON` class |
| 04 Blender exporter | Mostly gone. glTF and `.blend` import are built in |
| 06 Collision | Optional. A real 3D physics engine is already there |
| 10 Batching and culling | Mostly gone. `MultiMesh`, automatic frustum culling |
| 11 Shadow mapping | Gone. `DirectionalLight3D` with four cascades |
| 13 Audio streams | Half gone. Buses and mixing yes; the synth is still yours |
| 15 Makefile and platform ports | Gone. Export templates |

**What you still write, because it is the game and not the engine:**

Chapters 06, 08, 09, 10 and 13 of *this* series — the spline, the vehicle model,
the AI, the race rules, the terrain fit. Those are close to a straight
transliteration of the C, because there is no engine feature that does them.
That is the honest summary of what a general-purpose engine is: it owns the
parts that are the same in every game, and none of the parts that are not.

**What it costs:**

- **Speed of the scripting layer.** Typed GDScript is perhaps 3–10× slower than
  C for tight numeric loops, and the gap is largest exactly where this project
  is heaviest: per-sample spline searches and per-cell terrain fitting. Chapter
  16 measures it and moves three functions to C.
- **Control over allocation.** You cannot arena-allocate. You *can* avoid
  allocating in the hot path, and chapter 03 is about how.
- **Binary size and startup.** A raylib racer is a 2 MB executable that starts
  instantly. A Godot export is ~40 MB and takes a second to spin up the servers.
- **A large API you must learn rather than read.** The C engine is 4,200 lines;
  you can read all of it in a weekend. Godot is well over a million.

The trade is worth taking for this game. It would not be for the Pi console
build in `PLATFORM=drm`, where the C version renders a full field at 60 fps on
hardware Godot's Compatibility renderer struggles with — chapter 17 is honest
about that.

---

## Reading order for the code itself

If you want to read the project rather than these chapters, this order builds
knowledge without forward references:

1. `scripts/data/level_data.gd` — the data model everything else consumes.
2. `scripts/game/car_tuning.gd` — every handling number, with its units.
3. `scripts/engine/track_spline.gd` — the geometric query API.
4. `scripts/game/car_body.gd` — one tick of a vehicle.
5. `scripts/game/ai_driver.gd` — one tick of a driver.
6. `scripts/game/race_director.gd` — how a field of them becomes a race.
7. `scripts/engine/prop_batcher.gd` — level data to `MultiMesh`.
8. `scripts/main.gd` — the wiring, which now reads as a summary of everything.

---

## Exercises

1. **Trace one value.** Pick the player's throttle. Find every place it is read
   or written, from `Input.get_action_strength("throttle")` to the audio
   generator's engine gain. Write the chain down, and compare it with the C
   version's six hops. Which layer did Godot remove?

2. **Break the boundary deliberately.** Add
   `@onready var hud: Hud = get_node("../../UI/Hud")` to `race_director.gd` and
   call it from `_physics_process`. Now run the headless test suite from chapter
   15. Explain the error you get in terms of this chapter — and note that,
   unlike the C version's link error, this one only appears at runtime.

3. **Measure the tick.** Add a counter to `race_director.gd` that sums
   `_physics_process` calls and prints ticks-per-second every 60 frames. Run
   with vsync on, then with `Engine.max_fps = 0` and vsync off. Explain why the
   number is the same both times, and what would make it differ.

4. **Find the slow motion.** Set `Engine.physics_ticks_per_second = 1000` and
   watch the game run in slow motion rather than freeze. Which setting is
   producing that behaviour, and which C-series guard does it correspond to?

5. **Deferred deletion.** In `_free_stage`, delete the `remove_child` line so it
   only calls `queue_free`. Add a `print` to `RaceDirector._physics_process`
   showing `_level.display_name`. Swap circuits and read the output. Exactly how
   many extra ticks does the dead stage run, and why that many?

---

Next: [02 — Typed GDScript](02-typed-gdscript.md)
