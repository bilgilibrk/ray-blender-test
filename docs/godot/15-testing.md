# 15 — Testing without a window

> `tests/` · `tools/run_tests.sh`. Mirrors
> [C chapter 14](../learn/14-testing.md).

---

## The problem

A racing game is mostly systems that are hard to look at. Lap counting is a
state machine. The AI is a controller. The spline is a numerical method. You can
play the game and see that something is wrong; you cannot play the game and see
*which of six interacting systems* is wrong.

And the obvious approach — play it and check — does not scale: a three-lap race
takes three minutes, and the interesting cases (a car spun into a gravel trap on
the far side of the circuit on lap 2) take longer to *set up* than to observe.

The C series' answer is a headless test binary that simulates complete races
thousands of times faster than real time. Godot's `--headless` mode looks like
the same thing, and it is not — which is the substance of this chapter.

---

## What `--headless` actually gives you

```sh
godot --headless --path . --script res://tests/run_tests.gd
```

`--headless` selects a dummy display server and a dummy rendering driver.
Everything else runs: the scene tree, `_process`, `_physics_process`, timers,
signals, resource loading, the physics server, and the audio server (also a
dummy).

So "headless" in Godot means **the real game, minus pixels**. That is far more
than the C engine's headless build gets — it is the whole engine — and it comes
with a catch the C version does not have:

> The scene tree runs in real time. A three-lap race still takes three minutes.

The C test binary calls `RaceUpdate` in a loop and finishes a race in
milliseconds because nothing outside that loop needs to happen. Godot's tree
advances at wall-clock rate because the main loop is a main loop.

That single fact shapes the whole test strategy, and it is why this chapter has
two tiers.

---

## Tier 1: tests that need no tree at all

The fast tier. Everything here is pure objects and pure functions, runs in
milliseconds, and needs no `SceneTree`, no physics server, and no window.

What qualifies, from the previous chapters:

| System | Why it qualifies |
|---|---|
| `TrackSpline` | A `RefCounted` over packed arrays. No nodes anywhere |
| `Obb2` / `Manifold` | Pure maths |
| `LevelData`, `LevelJson` | Data and a parser |
| `AIDriver.think()` | Pure over (car state, tuning, spline, neighbours) |
| `CarBody.integrate()` | Pure over (input, surface, delta) — the node is only its container |
| `TerrainBuilder` | Pure over a spline |
| Level content invariants | Pure over `LevelData` |

That covers, by line count, most of the game. It qualifies **because of design
decisions made in earlier chapters** — the AI being a `RefCounted` rather than a
node, the surface being a parameter rather than a lookup, the race director
taking its level as an argument rather than loading it. Testability was not
retrofitted; it is what those decisions bought.

### A harness in forty lines

The C series uses a tiny `CHECK` macro rather than a framework, on the grounds
that a test harness you can read in a minute is worth more than one with
features. The same argument holds here:

```gdscript
# tests/harness.gd
class_name TestHarness
extends RefCounted

## Deliberately tiny. A framework would give us setup/teardown, mocking and
## parameterised cases; what we actually need is "run these functions, count
## the failures, exit non-zero". gdUnit4 is a good choice when this stops
## being enough — see the note below.

var checks: int = 0
var failures: int = 0
var _suite: String = ""

func suite(name: String) -> void:
    _suite = name

func check(condition: bool, message: String) -> void:
    checks += 1
    if condition:
        return
    failures += 1
    push_error("FAIL  %s: %s" % [_suite, message])
    printerr("FAIL  %s: %s" % [_suite, message])

func near(actual: float, expected: float, tolerance: float,
          message: String) -> void:
    check(absf(actual - expected) <= tolerance,
          "%s (got %.6f, expected %.6f +/- %.6f)"
                % [message, actual, expected, tolerance])

func report() -> int:
    print("%d checks, %d failures" % [checks, failures])
    return 0 if failures == 0 else 1
```

```gdscript
# tests/run_tests.gd
## Run: godot --headless --path . --script res://tests/run_tests.gd
extends SceneTree

func _init() -> void:
    var t := TestHarness.new()
    TestSpline.run(t)
    TestObb2.run(t)
    TestCar.run(t)
    TestAi.run(t)
    TestLevelInvariants.run(t, "res://levels/circuit01.track")
    TestLevelInvariants.run(t, "res://levels/circuit02.track")
    quit(t.report())
```

`extends SceneTree` with `--script` makes this the main loop: no scene is
loaded, `_init` runs, and `quit(code)` sets the process exit status, which is
what CI reads.

**When to use a framework instead.** gdUnit4 and GUT both give you assertion
libraries, test discovery, `await`-friendly integration tests, fixtures, and CI
reporters. They are worth adopting the moment you want tier-2 tests (below) with
`await`, or JUnit XML output for a CI dashboard. The forty lines above are what
you start with, not what you finish with.

### Unit tests with known answers

The best tests are the ones where the right answer comes from somewhere other
than the code under test:

```gdscript
# tests/test_spline.gd
class_name TestSpline

static func run(t: TestHarness) -> void:
    t.suite("spline")

    # A circle has a known circumference, a known tangent at every point, and
    # a known nearest point. Three independent properties from one primitive
    # — and none of them derived from the implementation.
    var spline := TrackSpline.new()
    t.check(spline.build_from_level(_ring(10.0, 64, 0.69), 0.25), "ring builds")
    t.near(spline.length, TAU * 10.0, TAU * 10.0 * 0.005, "loop length")

    var q := SplineQuery.new()
    spline.closest_into(Vector3(9.0, 0.0, 0.0), -1, q)
    t.near(q.lateral, 1.0, 0.05, "one unit inside the ring is one unit right")

    # Wrapped differences, at the seam where they are hard.
    t.near(spline.wrap_delta(0.3, spline.length - 0.2), 0.5, 1e-3,
           "wrap_delta across the line")
    t.near(spline.wrap_delta(spline.length - 0.2, 0.3), -0.5, 1e-3,
           "and back the other way")
```

### Robustness tests

The C series exercises its JSON parser against hostile input. The Godot
equivalent is the tolerant loader from chapter 04, and the interesting cases are
the same:

```gdscript
# tests/test_level_json.gd
static func run(t: TestHarness) -> void:
    t.suite("level json")

    # Missing everything except waypoints must still load.
    var minimal: Dictionary = {"waypoints": [
        {"pos": [0, 0, 0]}, {"pos": [1, 0, 0]}, {"pos": [0, 0, 1]}]}
    var level: LevelData = LevelJson.to_level_data(minimal, "<test>", 12)
    t.check(level != null, "a bare waypoint ring loads")
    t.check(level.checkpoints.size() == 12, "gates are auto-generated")
    t.near(level.waypoint_widths[0], 0.69, 1e-6, "width falls back to default")

    # Two waypoints is not a loop.
    t.check(LevelJson.to_level_data({"waypoints": [{"pos": [0, 0, 0]}]},
            "<test>", 12) == null, "fewer than three waypoints is rejected")

    # JSON has one number type; 3 and 3.0 must behave identically.
    t.check(LevelJson.to_level_data(_ring_json(laps_value = 3), "<t>", 12).laps
            == LevelJson.to_level_data(_ring_json(laps_value = 3.0), "<t>", 12).laps,
            "int and float laps agree")

    # Hostile: wrong types where objects are expected.
    t.check(LevelJson.to_level_data({"waypoints": "not an array"},
            "<test>", 12) == null, "a string where an array belongs")
    t.check(LevelJson.to_level_data({"waypoints": [1, 2, 3]},
            "<test>", 12) == null, "numbers where objects belong")
```

That last pair matters more in Godot than in C, because chapter 04's loader is
the **only** place a `Variant` is allowed to exist. If it lets a malformed value
through, the type system downstream is a fiction.

### Invariants over content

This is the category the C series is most enthusiastic about, and it ports
completely, because it is pure data.

The level format deliberately keeps art, solidity, drivability and run-off as
four independent lists (chapter 04). They can disagree. Tests are what keep them
honest:

```gdscript
# tests/test_level_invariants.gd
static func run(t: TestHarness, path: String) -> void:
    t.suite("content: %s" % path.get_file())
    var level: LevelData = load(path) as LevelData
    var spline := TrackSpline.new()
    spline.build_from_level(level, 0.25)
    var q := SplineQuery.new()

    # No collider should sit on the racing surface: that would wall off the
    # track. Sample the centre line and check nothing solid overlaps the lane.
    var blocking: int = 0
    for i: int in spline.count:
        var p: Vector3 = spline.positions[i]
        for wall: LevelBox in level.colliders:
            if _box_overlaps_lane(wall, p, spline.widths[i] * 0.5):
                blocking += 1
                break
    t.check(blocking == 0, "%d centre-line sample(s) are blocked by a collider"
            % blocking)

    # A sand trap on the racing line is a trap you cannot avoid.
    var on_line: int = 0
    for i: int in spline.count:
        var p: Vector3 = spline.positions[i]
        if level.in_sandtrap(p.x, p.z):
            on_line += 1
    t.check(on_line == 0, "%d centre-line sample(s) sit in a sand trap" % on_line)

    # A trap 40 units from the track is scenery nobody will ever touch: either
    # a placement mistake or a waste.
    var adrift: int = 0
    for trap: LevelBox in level.sandtraps:
        spline.closest_into(trap.center, -1, q)
        if absf(q.lateral) > q.half_width + 6.0:
            adrift += 1
    t.check(adrift == 0, "%d sand trap(s) are nowhere near the track" % adrift)

    # Every gate must be reachable in order, and roughly evenly spaced.
    var gaps: PackedFloat32Array = _gate_gaps(level, spline)
    t.check(gaps.min() > 2.0, "gates are at least 2 units apart")
    t.check(gaps.max() < spline.length * 0.5, "no half-lap gap between gates")

    # The grid must face down-track, or the race starts with a six-car pile-up
    # facing the wrong way. This is the test that catches a broken yaw
    # convention (chapter 05) before a human ever sees it.
    for i: int in level.spawns.size():
        var spawn: LevelSpawn = level.spawns[i]
        spline.closest_into(spawn.position, -1, q)
        var facing: Vector2 = Convention.forward(spawn.yaw)
        var down_track := Vector2(q.tangent.x, q.tangent.z)
        t.check(facing.dot(down_track) > 0.7,
                "spawn %d faces down-track (dot %.2f)"
                        % [i, facing.dot(down_track)])
```

These are not tests of the code. They are **tests of the content**, run against
the shipped circuits, and they catch a category of bug that no amount of unit
testing reaches: a level that is technically valid and unplayable.

The spawn-facing check is the highest-value one in the file. Chapter 05's π
difference between the two engines' yaw conventions is exactly the sort of thing
that survives review, and this test fails loudly the moment it is wrong.

### Testing the vehicle in isolation

```gdscript
# tests/test_car.gd
static func run(t: TestHarness) -> void:
    t.suite("car")
    var tuning := CarTuning.new()
    var surface := CarSurface.new()      # defaults: tarmac, flat
    var input := CarInput.new()

    # A CharacterBody3D that is never added to the tree. integrate() touches
    # no node state, which is exactly what makes this test possible — see
    # chapter 08.
    var car := CarBody.new()
    car.tuning = tuning
    car.reset_to(Vector2.ZERO, 0.0)

    # Full throttle on the flat must approach top speed and not exceed it.
    input.throttle = 1.0
    for i: int in 1200:                 # ten seconds at 120 Hz
        car.integrate(input, surface, 1.0 / 120.0)
    t.near(car.forward_speed, tuning.top_speed, 0.15, "reaches top speed")
    t.check(car.forward_speed <= tuning.top_speed + 1e-3, "does not exceed it")

    # In a gravel trap, full throttle settles near a seventh of racing pace.
    surface.speed_scale = tuning.sand_speed_scale
    surface.drag = tuning.sand_drag
    surface.grip = tuning.grip_sand
    for i: int in 1200:
        car.integrate(input, surface, 1.0 / 120.0)
    t.check(car.forward_speed < 1.1, "gravel holds the car under 1.1 u/s")

    # Frame-rate independence: the same ten seconds at three tick rates must
    # agree. This is the test that catches an exp() replaced by a lerp().
    var speeds: PackedFloat32Array = []
    for hz: float in [30.0, 60.0, 240.0]:
        var probe := CarBody.new()
        probe.tuning = tuning
        probe.reset_to(Vector2.ZERO, 0.0)
        var flat := CarSurface.new()
        for i: int in int(hz * 10.0):
            probe.integrate(input, flat, 1.0 / hz)
        speeds.append(probe.forward_speed)
        probe.free()
    t.near(speeds[0], speeds[2], 0.05, "30 Hz and 240 Hz agree")
    t.near(speeds[1], speeds[2], 0.02, "60 Hz and 240 Hz agree")
    car.free()
```

`CarBody.new()` without adding it to the tree gives you a node whose script
state is live and whose engine-side node state is inert. `integrate()` works;
`move_and_slide()` would not. That is a clean seam and it is worth designing
for — the moment `integrate` reads `global_position`, this test stops working.

`free()`, not `queue_free()`: chapter 03's rule for an object that was never in
the tree.

The frame-rate independence test is the most valuable in the file for the least
effort. It is three lines and it catches every future regression where somebody
writes `velocity *= 0.98` instead of `velocity *= exp(-k * delta)`.

---

## Tier 2: the whole game, headless

Tier 1 cannot tell you whether a *race* works. For that you need the real scene,
the physics server and the race director, and you need to accept wall-clock
time.

```gdscript
# tests/integration/test_full_race.gd
## Runs a complete six-car race with the player on autopilot and asserts on
## the outcome. Takes about as long as the race does, so this is a nightly or
## pre-merge job, not something to run on every save.
extends Node

const LAPS: int = 2

func _ready() -> void:
    var main: Node = preload("res://scenes/main.tscn").instantiate()
    add_child(main)
    var race: RaceDirector = main.get_node("Stage/RaceDirector")
    race.autopilot = true
    race.total_laps = LAPS

    await race.state_changed_to(RaceDirector.State.FINISHED)

    var harness := TestHarness.new()
    harness.suite("full race")
    for i: int in race.racer_count():
        var p: RaceProgress = race.progress_of(i)
        harness.check(p.finished, "racer %d finished" % i)
        harness.check(p.lap == LAPS + 1, "racer %d completed every lap" % i)
        harness.check(p.best_lap_time > 20.0 and p.best_lap_time < 90.0,
                "racer %d lap time is plausible (%.1f)" % [i, p.best_lap_time])
    harness.check(race.rescue_count() < 4,
            "at most three rescues across the whole field (%d)"
                    % race.rescue_count())
    get_tree().quit(harness.report())
```

Note what is asserted: **behaviour, not positions.** "Everybody finished", "laps
are in a plausible range", "the field did not need rescuing repeatedly". Those
survive a physics engine update, a Godot version bump, and a platform change.
Asserting that car 3 is at (12.44, 0.0, −8.71) after 4,000 ticks does not, for
the reasons chapter 07 sets out about solver determinism.

`rescue_count()` is a particularly good canary: rescues are the failure valve, so
a change that makes the AI slightly worse at a corner shows up as a rescue count
going from 1 to 9 long before it shows up as anything visible.

### Making tier 2 fast, and what it costs

Three ways to accelerate a real-time test, in increasing order of how much they
change what you are testing:

**1. Fewer laps.** Two laps instead of three still exercises every gate, every
lap transition and the finish. Free.

**2. `Engine.time_scale`.** Multiplies simulated time against wall-clock, so
more physics ticks happen per real second. Raise
`Engine.max_physics_steps_per_frame` to match or the tick rate silently caps and
the race runs in slow motion relative to the scale you asked for. Fidelity is
unchanged — the tick size is the same — but the machine has to keep up, so the
practical ceiling is what your CPU can do.

**3. Drop the physics server.** Run the simulation with chapter 07's custom
`Obb2` collision instead of `move_and_slide`, in a plain loop with no tree:

```gdscript
# tests/sim/headless_race.gd
## A whole race with no SceneTree, at whatever rate the CPU manages — the C
## engine's model, and thousands of times faster than real time. Possible only
## because CarBody.integrate() and AIDriver.think() are pure, and because the
## static collision path can be swapped for the hand-written one.
func run(level: LevelData, ticks: int) -> RaceDirector:
    var race := RaceDirector.new()
    race.collision_mode = RaceDirector.CollisionMode.CUSTOM_SAT
    race.setup(level, _spline, null, 6)
    race.autopilot = true
    for i: int in ticks:
        race._physics_process(1.0 / 120.0)
    return race
```

That is the C engine's test architecture, recovered — and it is only available
because chapter 07 kept the hand-written collision path as a live option rather
than deleting it.

Which is the honest summary of that chapter's fork in the road: **the reason to
own your collision is testability, and the price is owning your collision.** If
your project's tests can live with real-time integration runs, use
`move_and_slide` and never look back.

---

## Determinism

Three sources of non-determinism, two of which earlier chapters have already
removed:

**Wall-clock time in the simulation.** Chapter 09's AI accumulates `delta`
rather than calling `Time.get_ticks_msec()`, so the wobble term is reproducible.
The C version does not, and its testing chapter documents that as the one crack
in its determinism. Fixed for free during the port.

**Unseeded randomness.** GDScript's global `randf()` is seeded from the system
RNG at startup. Anything in the simulation that needs randomness must own a
`RandomNumberGenerator` with an explicit seed:

```gdscript
var _rng := RandomNumberGenerator.new()
func _init(seed_value: int) -> void:
    _rng.seed = seed_value
```

**Float determinism across platforms.** Not guaranteed, in Godot or anywhere
else without deliberate effort. `Vector3` components are 32-bit in a standard
build while GDScript's `float` is 64-bit, so mixed expressions round in
platform-dependent ways. A same-machine, same-build race is reproducible; a
cross-platform bit-exact one is not, and no amount of care in your code changes
that.

So: assert on ranges and invariants, not on bits. If you need bit-exact
cross-platform behaviour — lockstep networking — that is a fixed-point
simulation and a much larger project.

---

## What is not tested, and how to notice

Honesty about the gaps is more useful than a coverage number:

| Not tested | Why | Mitigation |
|---|---|---|
| Rendering output | No rendering driver under `--headless` | Screenshot comparison in a separate GPU job |
| Shader compilation | Same | Chapter 12's smoke scene under each driver |
| Audio output | Dummy driver | Nothing automated; listen |
| Input mapping | No devices | A manual checklist per release |
| Editor tooling (`@tool` scripts) | Runs in the editor, not the game | `--headless --editor --quit` catches parse errors |
| Anything visual about the AI | "It finished" is not "it looked good" | Watch a replay |

That last row deserves emphasis. The tier-2 test asserts that every car
finishes. It cannot tell you the AI spent lap 2 grinding along a barrier at
walking pace, because that still finishes. The rescue count catches the worst of
it; watching catches the rest.

---

## Wiring it into CI

```yaml
# .github/workflows/test.yml (sketch)
- name: Parse every script with warnings as errors
  run: godot --headless --path . --quit

- name: Unit tests
  run: godot --headless --path . --script res://tests/run_tests.gd

- name: Shader smoke test
  run: xvfb-run -a ./tools/check_shaders.sh
```

The first line is doing more work than it looks. With chapter 02's warning
settings promoted to errors, importing the project parses every script and fails
the build on an untyped declaration, an unsafe property access, or a narrowing
conversion. **That is the typed-GDScript rule, enforced by CI, in one command.**

The third needs a virtual display because a GL context needs one even when
nothing is shown — see chapter 12.

---

## Tests as a tuning instrument

The C series makes a point worth repeating: these tests were not written after
the fact to protect working code. Several of them *found the tuning*.

The gravel-trap assertion — "full throttle settles under 1.1 u/s" — is how
chapter 08's three sand numbers were chosen. The requirement was stated as a
number, the test enforced it, and the parameters were adjusted until it passed.
The test is now a regression guard, but it started as a specification.

Same for the frame-rate independence test, which is how the `exp()` decay
replaced a naive multiply, and the spawn-facing test, which is how the yaw
convention was pinned during the port.

> A test that encodes a design requirement is worth more than a test that
> encodes current behaviour. The first tells you what the game is supposed to
> be; the second only tells you it changed.

---

## Exercises

1. **Break frame-rate independence.** Replace `exp(-lateral_grip * delta)` with
   `(1.0 - lateral_grip * delta)` and run the car tests. Which assertion fails
   first, and at which tick rate? Now do the same to the camera smoothing and
   note that *nothing* fails — what test would you add?

2. **Break the content.** Move one barrier onto the racing line in
   `circuit01.track` and run the invariant tests. Read the failure message. Is
   it enough to find the barrier? Improve it so it prints the world position.

3. **Time the tiers.** Measure tier 1 and tier 2 wall-clock. Then implement the
   `CollisionMode.CUSTOM_SAT` path and measure a full race with no tree.
   Decide, with numbers, which tier belongs on every commit.

4. **Find the crack.** Replace the AI's `_clock` with
   `Time.get_ticks_msec() / 1000.0`, run the same headless race twice, and diff
   the finishing order and times. How many laps before the two runs disagree?

5. **A test that would have caught a real bug.** Chapter 08 describes cars
   driving backwards off the grid because the countdown fed them a full brake.
   Write the test that catches it. What does it assert, and in which tier does
   it belong?

6. **Coverage of the untested.** Pick one row from the "not tested" table and
   automate it. Screenshot comparison is the interesting one: what tolerance do
   you need before it stops failing on a driver update, and is it still useful
   at that tolerance?

---

Next: [16 — Writing the hot paths in C](16-native-c.md)
