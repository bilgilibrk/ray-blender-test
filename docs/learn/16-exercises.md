# 16 — Exercises

> Graded projects, from a one-line tweak to a new subsystem.

Each chapter ends with exercises tied to its own material. This chapter is for
larger work: projects that cross subsystem boundaries, where the interesting
part is deciding *where* the change belongs.

Every project lists:

- **What to build**
- **Where it touches** — the files you will end up in
- **How you know it works** — the check, ideally an automated one
- **The trap** — what usually goes wrong

Work in a branch. `make test` before and after, every time.

---

## Getting oriented

Before any of this, run these and read the output:

```sh
make test --verbose             # 414 checks plus telemetry
./build/desktop/racer --debug   # F1 overlay on
./build/desktop/racer --autopilot --debug    # watch the AI
make shaders                    # both GLSL variants
./build/desktop/racer --frames 1             # read the load-time log
```

The last one is the most underrated. The startup log tells you arena usage,
chunk counts, triangle counts, grid dimensions, light counts and the elevation
range — a complete picture of what the level cost.

---

## Tier 1 — One change, one file

Half an hour each. The point is to close the loop: change something, rebuild,
see it, measure it.

### 1.1 Retune the car

Change one value in `CarDefaultTuning()`, run `make test`, record the lap time.
Do it for `enginePower`, `gripTarmac`, `maxYawRate` and `dragQuadratic`.

Build a small table: parameter, old value, new value, circuit01 best lap,
leader average speed, worst off-track %. Which parameter has the most leverage
on lap time? Which has the most on *feel*, judged by driving?

**The trap:** changing two things at once. You learn nothing.

### 1.2 A third surface

Add ice: high top speed, near-zero grip. Chapter 07's exercise 3 has the shape.
Add `"icepatches"` to the level format, a loader branch, a surface selection in
`race.c`, and a `SurfaceRun` test asserting it is fast and uncontrollable.

**Where:** `car.h`, `car.c`, `level.h`, `level.c`, `race.c`, `test_race.c`.

**The trap:** forgetting `EstimateLevelBytes`. Chapter 02 explains why it does
not actually break — work out why before you look.

### 1.3 Make the night darker

`NightSettings` in `main.c` scales four values. Push it further: a real
moonlit night where the headlights genuinely carry the scene. Then check the
lamp posts still read as warm pools rather than blown-out discs.

**The trap:** scaling `groundColor` as well. Chapter 12 explains why vertex
colours can only subtract — work out what happens to the relief shading if the
material colour goes dark too.

### 1.4 Camera personality

Add `--camera chase|overhead|cinematic` selecting three different
`ChaseCamera` configurations. Overhead: north-up, high, no look-ahead.
Cinematic: low, far back, slow yaw smoothing.

**The trap:** the low camera will show you that the world has no skybox and the
terrain ends. Decide whether that is a bug to fix or a reason the camera is
high.

---

## Tier 2 — One feature, several files

An afternoon each. These require deciding where code belongs.

### 2.1 Sector times

The gates are already resolved to arc lengths. Record the time at each, keep the
best per sector, and show the current sector's delta against your best on the
HUD — green if ahead, red if behind.

**Where:** `race.h` (state), `race.c` (`UpdateCheckpoints`, `RaceReset`),
`hud.c` (display).

**Check:** add a test asserting the sum of a lap's sector times equals its lap
time to within a tick.

**The trap:** resetting sector state. `RaceReset` and `RaceRespawn` are different
situations — one should clear bests, one should not.

### 2.2 A time-trial mode

`--timetrial`: one car, no countdown, no opponents, unlimited laps, and a ghost
of your best lap. The ghost is a recorded array of `(time, position, yaw)`
samples replayed and drawn translucent.

**Where:** `main.c` mostly, plus a new `game/src/ghost.c`.

**Check:** the ghost must be frame-rate independent — record against
`race.elapsed`, not frame numbers, and interpolate on playback.

**The trap:** recording every physics tick. At 120 Hz for a 23-second lap that
is 2,760 samples per lap. Fine — but decide it deliberately rather than
discovering it.

### 2.3 Weather

A `--rain` mode: darker sky, higher fog, reduced `gripTarmac`, and a wet-line
effect where the racing line has more grip than the rest of the lane.

**Where:** `car.h` (surface), `race.c` (surface selection using `q.lateral`),
`main.c` (render settings).

**Check:** a surface test asserting wet grip sits between dry tarmac and grass.
Then a race simulation asserting lap times rise by a plausible margin — and that
every car still finishes.

**The trap:** the AI does not know about rain. Its `targetSpeed` heuristic will
have it arriving at corners too fast. Decide whether to tell it (a parameter on
`AIThink`) or to let it struggle (which is arguably more realistic).

### 2.4 A pit lane

`roadPitEntry`, `roadPitStraight`, `roadPitGarage` and `pitsOffice` are all in
the kit and already placed on circuit01. Make them functional: a branch off the
main line where a car can stop, wait three seconds, and rejoin with fresh grip.

**Where:** this is the hardest Tier 2 project, because the spline is a *single
closed loop*. A pit lane is a second path. Options: a second spline with entry
and exit arc lengths on the main one; or a "pit corridor" defined by boxes with
special surface rules and no spline at all.

**Check:** a car that pits must not gain a lap. `test_race.c`'s finishing-order
invariants will tell you if it does.

**The trap:** `SplineClosest` will snap a car in the pits to the main line,
because the pit lane runs parallel to it. Chapter 05's coarse-sweep discussion is
directly relevant.

---

## Tier 3 — A subsystem

A weekend or more. Each has a real design decision at its centre.

### 3.1 Replays

Record the whole race and play it back. Two architectures, and choosing between
them is the exercise:

**Input recording** — store `(tick, carIndex, CarInput)` and re-simulate.
Tiny files, exact. Requires *perfect determinism*: the `GetTime()` call in
`ai.c` that Chapter 14 used to call the one crack is now closed, and
`RunDeterminismTests` holds it closed — but there may be others. Any
floating-point difference compounds and the replay diverges.

**State recording** — store every car's position, yaw and speed every N ticks
and interpolate. Robust, larger files, cannot be re-simulated with different
physics.

Build one. Write down why. Then, if you chose input recording, prove determinism:
run the same inputs twice and compare the final state bit-for-bit.

**Where:** a new `game/src/replay.c`, hooks in `race.c` and `main.c`.

### 3.2 Split screen

Two players, two cameras, two viewports, one race.

**Where:** `main.c` (two `ChaseCamera`s, `rlViewport`), `input.c` (a second
binding set), `race.c` (a second player index), `hud.c` (per-viewport layout).

**The interesting problem:** there is one shadow map, centred on one focus.
Options: widen the box to cover both cars (loses resolution — how much?); render
two shadow passes (doubles the cost); or accept shadows following player 1 only.
Measure before deciding.

**Check:** FPS with `--no-vsync`, before and after. Split screen roughly doubles
the fragment work.

### 3.3 A different game on the same engine

The strongest test of whether `engine/` is really game-agnostic: build something
that is not a racer.

A top-down shooter needs: level loading ✓, collision ✓, static batching ✓,
lighting ✓, terrain ✓, audio ✓, input ✓, camera ✓. It does *not* need the
spline, and it *does* need projectiles and multiple collision layers.

Write `game2/` with its own `main.c`, link against `engine/`, and note every
place you had to reach into engine code and change something. Each one is a
place the abstraction leaked.

**Prediction:** you will need dynamic colliders (`CollisionWorld` is
build-once), and a way to remove a prop from a static batch. Both are real
limitations, both are stated in the chapters, and finding them yourself is
the point.

### 3.4 Networked multiplayer

The deep end.

The simulation is already fixed-step and nearly deterministic, which makes
**lockstep** viable: exchange inputs, simulate identically on every machine.
That is why fighting games and RTSs use it. But it requires *bit-identical*
floating point across machines — different compilers, different `-ffast-math`
settings, different libm implementations of `sinf` all break it.

The alternative is **client-server with prediction and reconciliation**: the
server is authoritative, clients predict locally and correct when the server
disagrees.

Implement lockstep for two players on a LAN first, because the codebase is
already 90% of the way there. Then find out empirically whether the determinism
holds. Chapter 14's exercise 5 is the prerequisite.

**Where:** a new `net/` module, changes to `main.c`'s loop structure, and a
delay-based input buffer.

### 3.5 An in-game editor

The Blender add-on is the real editor, but an in-game one closes the loop
differently: drive to a spot, place a barrier, drive into it.

**Where:** a new mode in `main.c`, ray-casting from the camera to the ground
plane, and a JSON *writer* (Chapter 03's exercise 6).

**The interesting problem:** placing a prop requires rebuilding the static batch,
which takes long enough to hitch. Options: a separate unbatched "editing" draw
path for new props; rebuilding only the affected chunk; or accepting the hitch.
The first is what real editors do.

---

## Tier 4 — Research projects

No single right answer. These are where you would end up if this were your
codebase and you kept going.

### 4.1 Make the AI faster than the player

The current AI laps circuit01 in 23.3 s. A skilled human can beat that.

Approaches, roughly in order of ambition:

- **Better line.** Implement true out-in-out. Measure.
- **Offline optimisation.** Precompute an optimal racing line by minimising lap
  time over the track's width, using the physics model as the constraint. Store
  it as a lateral-offset-per-arc-length table in the level file.
- **Learn it.** Run thousands of headless races with perturbed parameters and
  keep what is fastest. `test_race.c` already shows how to run a race in
  milliseconds — this is a search problem with a fast evaluator.

**Constraint:** the AI must still use `CarInput`. No cheating.

### 4.2 Cascaded shadow maps

Chapter 11's exercise 6. The current single 34-unit box follows the player and
everything outside is unshadowed.

Implement two or three cascades. The design questions: how do you split by
depth? How do you avoid a visible seam at the boundary? And — the one that
actually decides it — does it fit in the GLES2 uniform and texture-unit budget
documented in Chapter 15?

Write down the answer for both desktop and DRM. They may differ, and a feature
that only works on one is a legitimate outcome if you say so.

### 4.3 Deferred or clustered lighting

The current forward renderer uploads eight lights per draw and picks them per
chunk (Chapter 10). It is a good design for this content — few lights, low
overdraw, weak hardware.

Implement clustered forward: divide the view frustum into a 3D grid, assign
lights to clusters once per frame, and have the fragment shader look up its
cluster. Then measure with 200 lights instead of 44.

The honest question to answer: **at what light count does the new system win?**
If the answer is "more lights than any level has", that is a real result and
worth writing down.

### 4.4 Procedural circuits

`build_demo_track.py` builds a circuit from a hand-written list of moves and
solves two unknowns to close the loop (Chapter 04). Generate the move list
instead.

The constraints are already encoded and will fight you: the loop must close in
plan and in elevation, the net turning must be ±360°, the track must not run
within `LOOP_CLEARANCE` of itself, the tightest corner must not fall below
`maxYawRate × R = 1.8 u/s`, and every centre-line point must land on a placed
tile.

`test_race.c` is your fitness function. Generate a hundred circuits, keep the
ones that pass, and look at what they have in common.

**The interesting part:** passing the tests is not the same as being *good*. A
circuit of twelve identical corners passes everything. What makes a layout worth
driving, and can you measure it? (Start with the "% of the lap that is
corner-limited" number the tests already print.)

---

## How to work on this codebase

Some habits that the code itself demonstrates:

**Read the comments about why.** The valuable ones in this codebase are not
descriptions of what the code does — they are records of what was tried and
failed. `SettleToSurface`'s rate-limit comment, `SplineClosest`'s coarse sweep,
`CarHold`'s countdown note, `RenderModelTransform`'s material save/restore. Each
is a bug that will come back if the comment is ignored.

**Run the tests constantly.** Two seconds. There is no reason not to.

**Change one thing.** The codebase has ~40 tuning constants that interact. Two
changes at once teaches nothing.

**Write the invariant before the feature.** Chapter 14's third category — the
content invariants — are all cheaper to write before the content exists than to
retrofit.

**Say why in the commit.** The git log here reads as sentences:
`Steer right when the input says right`, `Hold the grid still during the
countdown instead of braking it`, `Keep the cars on top of the road going
uphill`. Each names the *symptom*, which is what you will search for when it
recurs.

**Measure before optimising.** The `F1` overlay gives FPS, drawn chunks, total
chunks, triangles, lights and skid marks. The startup log gives arena usage and
grid sizes. `make test` gives lap times and surface distances. All of it existed
before it was needed, which is why it was there when it was.

---

## Where to go next

If this codebase was useful, the natural follow-ons:

- **raylib's own examples** — `vendor/raylib/examples/` is 200+ single-file
  programs, many under 100 lines.
- **Handmade Hero** — a game and engine written from scratch on video, and the
  clearest available demonstration of the "no framework" approach.
- **Real-Time Rendering** (Akenine-Möller et al.) — the reference for
  everything in Chapters 10 and 11, in far more depth.
- **Game Physics Engine Development** (Millington) — where to go after
  Chapter 06 if you want real 3D rigid bodies.
- **The Nature of Code** (Shiffman) — free online, and the gentlest good
  introduction to simulation.
- **Fabien Sanglard's code reviews** — the same "read a real codebase carefully"
  approach applied to Doom, Quake and Wolfenstein.

---

Back to the [index](README.md).
