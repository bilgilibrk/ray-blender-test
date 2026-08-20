# 10 — Race rules and state

> `scripts/game/race_director.gd` · `scripts/game/race_progress.gd` ·
> `scripts/game/racer.gd`. Mirrors
> [C chapter 09](../learn/09-race-rules.md).

---

## The problem

Six cars go round a loop three times. Somebody has to decide who is winning.

That sounds trivial and is not, because every obvious implementation breaks in a
way you can reproduce:

- **Count finish-line crossings.** A car shoved sideways across the line during
  a scrap gains a lap. A car that clips the very edge of the line trigger
  crosses twice in one tick.
- **Count checkpoint crossings geometrically.** Same problem, twelve times over,
  plus a gate wide enough not to be missed is wide enough to be crossed
  sideways.
- **Order by distance to the next checkpoint.** Correct except at the instant a
  car passes one, where it jumps from "nearly there" to "furthest away" and the
  standings flip.
- **Order by total distance travelled.** A car doing donuts wins.

What works is the thing the spline already gives you: **arc length round the
loop**, which is monotonic, continuous, and immune to being pushed sideways.
Gates are then enforced *in order* on top of it, so that cutting the course does
not advance a lap.

> Progress is measured as arc length along the centre line rather than by
> geometric gate crossings. The gates are still enforced in order, so cutting
> the course does not advance a lap.

---

## The state

```gdscript
# scripts/game/race_progress.gd
class_name RaceProgress
extends RefCounted

## One car's standing in the race. A record, so every field is typed and a
## typo is a parse error — see chapter 02 on why this is not a Dictionary.

var lap: int = 0                    ## times the finish line has been crossed
var next_checkpoint: int = 0
var spline_hint: int = -1
var spline_distance: float = 0.0
var last_spline_distance: float = 0.0
## Monotonic distance round the loop, for sorting. Grows without bound.
var score: float = 0.0

var lap_start_time: float = 0.0
var last_lap_time: float = 0.0
var best_lap_time: float = 0.0
var finish_time: float = 0.0
var finished: bool = false
var finish_position: int = 0

var off_track_time: float = 0.0
var stuck_time: float = 0.0
```

```gdscript
# scripts/game/racer.gd
class_name Racer
extends RefCounted

## Everything the race knows about one entrant. The CarBody is a node; the
## driver, the progress and the input are not, because they have no transform
## and no tick of their own.

var car: CarBody = null
var input: CarInput = null
var progress: RaceProgress = null
var ai: AIDriver = null

var is_player: bool = false
var model: String = "raceCarRed"
var tint: Color = Color.WHITE
var display_name: String = "YOU"
```

`spline_hint` living in `RaceProgress` is the detail that makes chapter 06's
windowed search work: each car carries its own cache of where it was on the
spline last tick. Six cars, six hints, no interference. Put it in a single
global and two cars on opposite sides of the circuit destroy each other's
locality every tick.

`score` is the sort key and it is worth being precise about:

```gdscript
progress.score = float(progress.lap) * spline.length + _lap_relative(query.distance)
```

Lap number times loop length, plus how far round this lap. It grows
monotonically for the whole race, so comparing two cars is one float comparison
and there is no wrap-around case to get wrong.

---

## The director

```gdscript
# scripts/game/race_director.gd
class_name RaceDirector
extends Node

## Race session: the field of cars, their progress round the circuit, and the
## countdown/running/finished state machine.
##
## A plain Node, not a Node3D: it has no transform. It owns the racers, calls
## the AI, drives the cars, and announces what happened. It does not know
## whether anything is watching.

enum State { COUNTDOWN, RUNNING, FINISHED }

const MAX_RACERS: int = 8
const COUNTDOWN_SECONDS: float = 3.6
const OFFTRACK_RESPAWN: float = 6.0     ## seconds stranded before a rescue
const STUCK_RESPAWN: float = 4.0
## A gate counts as passed when the car's arc length crosses it moving forward.
## The window rejects the huge jump a wrap-around or a respawn produces.
const GATE_WINDOW: float = 3.0

signal state_changed(state: State)
signal lap_completed(racer_index: int, lap: int, lap_time: float)
signal racer_finished(racer_index: int, position: int, total_time: float)
signal racer_rescued(racer_index: int)
signal countdown_tick(seconds_remaining: int)

@export var tuning: CarTuning
@export var autopilot: bool = false     ## hands the player's car to the AI

var state: State = State.COUNTDOWN
var countdown: float = COUNTDOWN_SECONDS
var elapsed: float = 0.0
var total_laps: int = 3
var finished_count: int = 0

var _level: LevelData = null
var _spline: TrackSpline = null
var _racers: Array[Racer] = []
var _standings: PackedInt32Array = []
var _checkpoint_distance: PackedFloat32Array = []
var _start_distance: float = 0.0
```

### Signals are the boundary

This is the Godot-specific design decision of the chapter, and it is what
chapter 01's headless test depends on.

The C version has `main.c` poll the `Race` struct every frame and hand values to
the HUD. Godot's idiom is the reverse: the race *announces*, and whoever cares
listens.

```gdscript
# scripts/main.gd
func _ready() -> void:
    race.lap_completed.connect(hud.on_lap_completed)
    race.racer_finished.connect(hud.on_racer_finished)
    race.racer_finished.connect(Audio.on_racer_finished)
    race.state_changed.connect(hud.on_state_changed)
    race.countdown_tick.connect(Audio.on_countdown_tick)
```

Three properties follow, and all three matter:

- **The race has no reference to the HUD.** It cannot call it, cannot break when
  it is absent, and can run in a test with nothing connected.
- **Two listeners cost the emitter nothing.** `racer_finished` reaching both the
  HUD and the audio system required no change to `race_director.gd`.
- **Connections die with their receivers.** A freed HUD's connection is removed
  automatically; the race does not hold it alive (chapter 03).

The rule for what becomes a signal: **events, not state.** "Lap completed" is an
event — it happens at an instant and a listener that misses it has missed it.
"Current lap number" is state, and the HUD should read it from
`race.progress_of(i).lap` in `_process`, not receive it. Emitting a signal per
field per tick is how a project acquires a hundred connections and a profiler
full of signal dispatch.

---

## Gates become arc lengths, once

```gdscript
func setup(level: LevelData, spline: TrackSpline, walls: StaticBody3D,
           racer_count: int) -> bool:
    _level = level
    _spline = spline
    total_laps = level.laps

    racer_count = clampi(racer_count, 1, mini(MAX_RACERS, maxi(1, level.spawns.size())))

    # Resolve each gate to an arc length once; progress is compared against
    # these. A geometric gate test would have to run every tick, per car; this
    # runs once per level and turns the whole question into float comparisons.
    _checkpoint_distance.resize(level.checkpoints.size())
    var scratch := SplineQuery.new()
    for i: int in level.checkpoints.size():
        _spline.closest_into(level.checkpoints[i].position, -1, scratch)
        _checkpoint_distance[i] = scratch.distance
    if not _checkpoint_distance.is_empty():
        _start_distance = _checkpoint_distance[0]
    ...
```

`closest_into(..., -1, ...)` — the hint is negative, forcing a full search. This
runs twelve times per level load, so precision is worth more than speed, and
using a stale hint here would silently place a gate on the wrong side of the
circuit.

Index 0 doubles as the finish line. That is a convention, not a property of the
data, and it is stated in `LevelData`'s doc comment so a level author knows the
first gate is special.

---

## Detecting a crossing

```gdscript
## Advances gate order and lap count from the car's arc-length progress.
func _update_checkpoints(racer: Racer) -> void:
    if _checkpoint_distance.is_empty():
        return

    var p: RaceProgress = racer.progress
    var target: float = _checkpoint_distance[p.next_checkpoint]

    var now: float = _spline.wrap_delta(p.spline_distance, target)
    var before: float = _spline.wrap_delta(p.last_spline_distance, target)

    # Forward zero-crossing of the signed distance to the gate.
    if before < 0.0 and now >= 0.0 and (now - before) > 0.0 \
            and (now - before) < GATE_WINDOW:
        var was_finish_line: bool = p.next_checkpoint == 0
        p.next_checkpoint = (p.next_checkpoint + 1) % _checkpoint_distance.size()
        if was_finish_line:
            _complete_lap(racer)
```

The whole gate system is that expression, so take it apart.

`wrap_delta(car, gate)` is the signed distance from the gate to the car, wrapped
into (−L/2, L/2]. Negative means the car has not reached it; positive means it
has passed. A crossing is therefore a **sign change from negative to positive**,
which is a zero crossing of a continuous function — and continuity is exactly
what arc length gives you and geometry does not.

**Why `(now - before) > 0`**: the car must be moving forward. A car reversing
across a gate produces a positive-to-negative crossing, which this rejects; a
car that reverses back over a gate it already passed does not un-pass it, but it
also does not re-pass it on the way forward, because `next_checkpoint` already
moved on. Reversing over the line and coming back does not gain a lap.

**Why the `GATE_WINDOW` of 3.0 units**: `wrap_delta` is discontinuous by
construction at the antipode — the point exactly half a lap away — where it
flips from +L/2 to −L/2. A car crossing the antipode produces an apparent
"crossing" of the gate with a delta of a full lap length. So does a respawn, and
so does a car being teleported to the grid.

3.0 units is generous: at 6.6 u/s and 120 Hz a car moves 5.5 cm per tick, so a
genuine crossing has a delta three orders of magnitude smaller than the window.
A tighter window would be equally correct and would leave less room for a frame
spike; a looser one would eventually admit a teleport. This is the sort of
constant that wants a comment saying what it is defending against, which is why
it has one.

### Completing a lap

```gdscript
func _complete_lap(racer: Racer) -> void:
    var p: RaceProgress = racer.progress
    var index: int = _racers.find(racer)

    if p.lap > 0:
        p.last_lap_time = elapsed - p.lap_start_time
        if p.best_lap_time <= 0.0 or p.last_lap_time < p.best_lap_time:
            p.best_lap_time = p.last_lap_time
        lap_completed.emit(index, p.lap, p.last_lap_time)

    p.lap += 1
    p.lap_start_time = elapsed

    if p.lap > total_laps and not p.finished:
        p.finished = true
        p.finish_time = elapsed
        finished_count += 1
        p.finish_position = finished_count
        racer_finished.emit(index, p.finish_position, p.finish_time)
```

`if p.lap > 0` is the off-by-one guard: crossing the line for the *first* time
is the start of lap 1, not the completion of lap 0, so no lap time is recorded
and no signal fires.

`p.lap > total_laps` rather than `>=`: with `total_laps = 3`, the car crosses
the line to start lap 1, 2, 3, and then a fourth time to finish. `lap` is 4 at
that point.

`finished_count` is incremented before being read, so the first car to finish
gets position 1.

---

## Standings

```gdscript
func _sort_standings() -> void:
    # Insertion sort: the field is tiny and nearly ordered every tick. An
    # O(n log n) sort would do more work setting up than this does finishing.
    for i: int in range(1, _racers.size()):
        var value: int = _standings[i]
        var j: int = i - 1
        while j >= 0 and _compare(_standings[j], value) > 0:
            _standings[j + 1] = _standings[j]
            j -= 1
        _standings[j + 1] = value

func _compare(a: int, b: int) -> int:
    var pa: RaceProgress = _racers[a].progress
    var pb: RaceProgress = _racers[b].progress

    # Finished cars always rank above unfinished ones, in finishing order.
    if pa.finished != pb.finished:
        return -1 if pa.finished else 1
    if pa.finished and pb.finished:
        return pa.finish_position - pb.finish_position
    if pa.score > pb.score:
        return -1
    if pa.score < pb.score:
        return 1
    # Stable tiebreak so identical scores do not swap places every tick.
    return a - b
```

Insertion sort on a nearly-sorted array of six is close to free: typically zero
or one swap per tick. It is also **stable**, which the final `return a - b`
reinforces — without a deterministic tiebreak, two cars with bit-identical
scores would swap every tick and the HUD would flicker.

Godot's `Array.sort_custom(Callable)` exists and would be the idiomatic choice.
It is not used here for two reasons: a `Callable` invocation per comparison is
considerably more expensive than an inlined comparison in a tight loop that runs
120 times a second, and `sort_custom` is not stable. For a list of six, hand
rolling it is both faster and more correct.

---

## Placing the grid

```gdscript
func _place_on_grid(index: int) -> void:
    var racer: Racer = _racers[index]
    var position := Vector2.ZERO
    var spawn_height: float = 0.0
    var yaw: float = 0.0

    if not _level.spawns.is_empty():
        var spawn: LevelSpawn = _level.spawns[index % _level.spawns.size()]
        position = Vector2(spawn.position.x, spawn.position.z)
        spawn_height = spawn.position.y
        yaw = spawn.yaw

    racer.car.reset_to(position, yaw)

    var p: RaceProgress = racer.progress
    p.reset()
    p.spline_hint = -1

    _spline.closest_into(Vector3(position.x, spawn_height, position.y),
                         -1, _scratch_query)
    racer.car.surface_height = _scratch_query.position.y
    racer.car.pitch = atan(_scratch_query.grade)
    p.spline_distance = _scratch_query.distance
    p.last_spline_distance = _scratch_query.distance
    # Still behind the line: one negative lap's worth of score, so a car that
    # has not yet crossed the start sorts below one that has.
    p.score = _lap_relative(_scratch_query.distance) - _spline.length
```

That last line is subtle and worth the comment it has. The grid sits *before*
the start line. `_lap_relative` measures from the line forward, so a car 2 units
behind it reads as `length - 2` — nearly a full lap ahead. Subtracting one loop
length puts it back where it belongs, at `−2`.

Get this wrong and the standings are correct from lap 1 onwards but nonsense on
the grid, which is exactly when the player is looking at them.

---

## Rescue

```gdscript
    if state == State.RUNNING and not p.finished:
        p.off_track_time = 0.0 if on_track else p.off_track_time + delta
        p.stuck_time = p.stuck_time + delta if racer.car.speed < 0.3 else 0.0
        if p.off_track_time > OFFTRACK_RESPAWN or p.stuck_time > STUCK_RESPAWN:
            _respawn(index)
```

```gdscript
## Puts a car back on the racing line facing the right way.
func _respawn(index: int) -> void:
    var racer: Racer = _racers[index]
    var here := Vector3(racer.car.plane_position.x, racer.car.surface_height,
                        racer.car.plane_position.y)
    _spline.closest_into(here, racer.progress.spline_hint, _scratch_query)

    racer.car.reset_to(
            Vector2(_scratch_query.position.x, _scratch_query.position.z),
            Convention.yaw_from_direction(_scratch_query.tangent.x,
                                          _scratch_query.tangent.z))
    racer.car.surface_height = _scratch_query.position.y
    racer.car.pitch = atan(_scratch_query.grade)
    racer.progress.off_track_time = 0.0
    racer.progress.stuck_time = 0.0
    racer.ai.recover_timer = 0.0
    racer_rescued.emit(index)
```

**Two timers, two thresholds.** Six seconds off track catches the gravel trap:
chapter 08 tuned the trap so that a car in it settles at 0.95 u/s, which is slow
enough that six seconds of trying is a fair punishment and short enough not to
be a walk. Four seconds stuck catches being wedged against a barrier, where
nothing is going to change by waiting.

Note the respawn deliberately does **not** reset progress. The car keeps its lap
count, its score, its next gate. Being rescued costs you time and position, not
your race. It also keeps its `spline_hint` — passing it into `closest_into` here
rather than −1 — because the nearest point to a car that has just spun off is
genuinely near where it was, and this is one of the few places where the hint's
locality is still valid.

**Rescue points the car down-track**, using chapter 05's convention helper. This
is one of the four places in the project that converts a direction into an
absolute yaw, and it is the one where getting it wrong is most visible: a
rescued car facing backwards drives into the field.

---

## The state machine, and pausing

```gdscript
func _physics_process(delta: float) -> void:
    _advance_state(delta)
    var locked: bool = state == State.COUNTDOWN

    for i: int in _racers.size():
        _tick_racer(i, locked, delta)

    _resolve_car_collisions()
    _sort_standings()

    if state == State.RUNNING and finished_count >= _racers.size():
        _set_state(State.FINISHED)

func _advance_state(delta: float) -> void:
    if state == State.COUNTDOWN:
        var before: int = int(ceil(countdown))
        countdown = maxf(0.0, countdown - delta)
        var after: int = int(ceil(countdown))
        if after != before:
            countdown_tick.emit(after)
        if countdown <= 0.0:
            _set_state(State.RUNNING)
    elif state == State.RUNNING:
        # Accumulated from the fixed step, not read from a clock: two runs of
        # the same race must produce the same lap times (chapter 15).
        elapsed += delta

func _set_state(next: State) -> void:
    if state == next:
        return
    state = next
    state_changed.emit(state)
```

`countdown_tick` fires on the integer boundary rather than every tick, so the
audio system gets exactly four events (3, 2, 1, 0) and does not have to
edge-detect them itself. Pushing that logic into the emitter is right when every
listener would otherwise do the same work.

### Pausing, the Godot way

The C version has `main.c` skip the simulation while `paused`. Godot has a
first-class mechanism, and using it correctly is one of the more common
stumbling blocks:

```gdscript
# scripts/main.gd
func toggle_pause() -> void:
    get_tree().paused = not get_tree().paused
```

`SceneTree.paused` stops `_process`, `_physics_process`, input callbacks and
timers for every node whose `process_mode` is `PROCESS_MODE_INHERIT` (the
default, inheriting from the parent) or `PROCESS_MODE_PAUSABLE`. Nodes set to
`PROCESS_MODE_ALWAYS` keep running.

So the pause menu itself, and the audio system, are `ALWAYS`:

```gdscript
# In the pause menu's _ready()
process_mode = Node.PROCESS_MODE_ALWAYS
```

and the race director is left at the default, which pauses it. That is the whole
implementation — no `if paused: return` anywhere in the simulation.

The trap: **`process_mode` is inherited from the parent by default.** A pause
menu that is a child of the world node inherits `PAUSABLE`, pauses with
everything else, and cannot unpause itself. Every project hits this once.

---

## The order of operations in one tick

The order inside `_tick_racer` is not arbitrary; several bugs live in permuting
it:

```gdscript
func _tick_racer(index: int, locked: bool, delta: float) -> void:
    var racer: Racer = _racers[index]
    var p: RaceProgress = racer.progress

    # 1. decide inputs
    if locked:
        # No input rather than a full brake: at a standstill the brake doubles
        # as reverse, so braking through the countdown drove the whole grid
        # backwards off the line. And clear the AI's stuck timer, or the field
        # launches already reversing (chapter 08).
        racer.input.clear()
        racer.ai.recover_timer = 0.0
    elif racer.is_player and not autopilot and not p.finished:
        racer.input.copy_from(player_input)
    else:
        var n: int = _fill_neighbours(index)
        racer.input.copy_from(racer.ai.think(racer.car, tuning, _spline,
                p.spline_hint, _neighbours, n, delta))
        if p.finished:
            racer.input.throttle = 0.0
            racer.input.brake = 0.35      # coast to a stop, do not freeze

    # 2. surface, from where the car is NOW
    _spline.closest_into(_car_point(racer.car), p.spline_hint, _scratch_query)
    p.spline_hint = _scratch_query.index
    _fill_surface(racer, _scratch_query)

    # 3. move
    if locked:
        racer.car.hold(_surface, delta)
    else:
        racer.car.integrate(racer.input, _surface, delta)
    racer.car.apply_motion()          # move_and_slide + contacts (chapter 07)

    # 4. progress, from where the car ENDED UP
    _spline.closest_into(_car_point(racer.car), p.spline_hint, _scratch_query)
    p.spline_hint = _scratch_query.index
    p.last_spline_distance = p.spline_distance
    p.spline_distance = _scratch_query.distance

    if state == State.RUNNING and not p.finished:
        _update_checkpoints(racer)
    p.score = float(p.lap) * _spline.length + _lap_relative(p.spline_distance)

    # 5. rescue
    ...
```

**Two spline queries per car per tick**, and the reason is in the comments: the
surface must be sampled where the car *is* (so the grip it experiences matches
the ground under it), and progress must be sampled where the car *ended up*
(so `last_spline_distance` → `spline_distance` is a genuine step and gate
crossings are not off by one tick). Using one query for both is a subtle
correctness bug that shows up as gates registering a tick late — invisible until
somebody complains that lap times are consistently 8 ms long.

**Car-versus-car resolution runs after every car has moved**, not inside the
loop. Resolving pairwise inside the loop would mean car 0 is pushed by car 1
before car 1 has moved this tick, which makes the result depend on grid index.

---

## Time formatting

```gdscript
## Formats seconds as m:ss.mmm.
static func format_time(seconds: float) -> String:
    if seconds <= 0.0 or seconds >= 5999.0:
        return "--:--.---"
    var minutes: int = int(seconds / 60.0)
    return "%d:%06.3f" % [minutes, seconds - float(minutes) * 60.0]
```

`%06.3f` is width six, three decimals, zero padded: `7.25` becomes `07.250`, so
`1:07.250` rather than `1:7.250`. The sentinel for "no time yet" is a formatted
string rather than an empty one, so the HUD's layout does not jump when the
first lap completes.

The upper bound catches uninitialised and infinite values before they reach the
screen as `16777216:00.000`.

---

## Swapping circuits

Chapter 01 covers the mechanics. The rule that matters here is what the race
director is allowed to assume:

```gdscript
## Everything per-level is passed in, not looked up. The director does not
## load a level, does not know a file path, and does not survive a circuit
## change — main.gd frees it and builds a new one.
func setup(level: LevelData, spline: TrackSpline, walls: StaticBody3D,
           racer_count: int) -> bool:
```

There is a tempting alternative — a `RaceDirector` autoload that reloads itself
between circuits, so the HUD's connections survive. Do not. It makes two
simultaneous races impossible, which chapter 15's tests need, and it turns
"which level are we on" into global state that every system can read and
therefore will.

Reconnecting a handful of signals after a circuit change is five lines in
`main.gd`. That is the correct price.

---

## Exercises

1. **Break the window.** Set `GATE_WINDOW` to 1000.0 and drive a lap. Then
   trigger a rescue on the far side of the circuit from the next gate. What does
   the lap counter do, and can you produce a car that finishes the race in one
   lap?

2. **Cut the course.** Drive across the infield, skipping four gates, and cross
   the finish line. Confirm the lap does not count. Now describe what the
   standings do while you are off-piste, and whether `score` is doing the right
   thing.

3. **Sector times.** Add per-gate split times: store the elapsed time at each
   gate crossing, emit a `sector_completed` signal with the delta to your best,
   and show it on the HUD. Which existing field makes this nearly free?

4. **Standings stability.** Remove the `return a - b` tiebreak and start two
   cars from identical spawns with `autopilot` on. Watch the position readout.
   How often does it flicker, and why does the insertion sort make it worse
   rather than better?

5. **The pause trap.** Make the pause menu a child of the world node with the
   default `process_mode`. Pause. Explain precisely why you cannot unpause, and
   which two nodes need `PROCESS_MODE_ALWAYS` for the pause screen to work.

6. **Two races at once.** Instance two `RaceDirector`s in one scene on two
   different circuits, with `autopilot` on for both. Confirm they do not
   interfere. Then find every piece of state that would have made this
   impossible if it had been an autoload.

---

Next: [11 — Rendering](11-rendering.md)
