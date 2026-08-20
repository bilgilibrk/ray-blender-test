# 18 — Exercises

> Graded projects, from a one-line tweak to a new subsystem. Mirrors
> [C chapter 16](../learn/16-exercises.md).

---

Each chapter ends with exercises about that chapter. These are about the whole
project: they cross files, they have design decisions in them, and several have
no single right answer.

They are graded by **how many files you have to understand**, not by how hard
the code is. Tier 1 changes one file. Tier 4 is a research project that could
reasonably take a month.

Every one of them has a way to tell whether you got it right, because "it
compiles and the game still runs" is not a result.

---

## Getting oriented

Before starting anything, three warm-ups that will save you time later.

**Turn the type checking on.** Copy chapter 02's `[debug]` block into
`project.godot`, restart the editor, and fix everything red. If you are working
from a fresh project rather than this one, this is where you discover how much
untyped GDScript you had.

**Run the tests.** `godot --headless --path . --script res://tests/run_tests.gd`.
They should pass in well under a second. If they do not, fix that before
changing anything — a failing baseline makes every later result ambiguous.

**Watch one number.** Add chapter 16's performance overlay and drive a lap
looking at it. Knowing that physics costs more than rendering on this project
changes which optimisations occur to you.

---

## Tier 1 — one change, one file

### 1.1 Retune the car

`scripts/game/car_tuning.gd` is a `Resource`, so you can duplicate it, tune the
copy, and swap between them at runtime without a restart.

Make three variants: a **kart** (low top speed, enormous grip, instant steering),
a **drifter** (low `grip_tarmac`, high `max_steer_low_speed`, high
`engine_power`) and a **truck** (heavy `drag_linear`, low `max_yaw_rate`, long
`wheelbase`).

*How to tell:* drive each for a lap and check the chapter 15 car tests still
pass with your resource substituted — the frame-rate independence one especially.
If a tuning change breaks it, you have introduced a `delta`-dependent term
somewhere.

### 1.2 A third surface

Add ice. Chapter 08 claims this needs three fields in `car_tuning.gd`, one
branch in `race_director.gd`, and nothing in `car_body.gd`.

*How to tell:* verify the claim. If you had to touch `car_body.gd`, the surface
abstraction has a hole and finding it is the actual exercise.

### 1.3 Make the night darker

Chapter 12's `TimeOfDay` swaps two `Environment` resources. Push the night one
until headlights are genuinely the only useful light, without the track becoming
unreadable.

*How to tell:* drive a lap at night with headlights off. You should be unable to
place the car. Turn them on and you should be able to drive, but not fast. If
you can drive fast with them off, it is not night.

### 1.4 Camera personality

`scripts/engine/chase_camera.gd` has three smoothing rates, a look-ahead and two
speed-reactive terms. Build three presets: **broadcast** (distant, slow, north-
up), **chase** (close, fast, rotating) and **hood** (very close, very fast).

*How to tell:* switchable at runtime with a key, and each should be usable for a
whole lap. If one makes you crash, work out whether that is the smoothing or the
look-ahead.

---

## Tier 2 — one feature, several files

### 2.1 Sector times

Split the lap into three sectors at gates 0, 4 and 8. Show the current sector
time on the HUD, coloured green or red against your best.

*Files:* `race_progress.gd` (three floats), `race_director.gd` (emit on gate
crossing), `hud.gd` (display).

*How to tell:* sector times must sum to the lap time, exactly, on every lap.
Assert it in a test. If they do not, you are measuring at a different point from
where you think.

### 2.2 A time-trial mode

One car, no countdown, no opponents, unlimited laps, and a **ghost** of your
best lap.

The ghost is the interesting half. Record `(time, position, yaw)` every physics
tick into a `PackedFloat32Array`, then play it back by interpolating between
samples. At 120 Hz a 60-second lap is 7,200 samples of five floats — 144 KB,
which is nothing.

*Files:* a new `time_trial_director.gd` (or a mode flag), `ghost_car.gd`,
`hud.gd`.

*How to tell:* the ghost must arrive at the line at exactly your recorded lap
time. If it drifts, you are interpolating against the wrong clock — see chapter
01 on `_process` versus `_physics_process`.

### 2.3 Weather

Rain that changes `grip_tarmac` from 9.5 to about 6.0 over thirty seconds, with
a matching visual: darker `Environment`, a particle system, and spray behind the
cars.

*Files:* `car_tuning.gd` or a new `weather.gd`, `race_director.gd` (surface
fill), `time_of_day.gd`, a `GPUParticles3D` scene.

*How to tell:* the AI should get slower without any change to `ai_driver.gd`,
because chapter 09's pace comes from a physical limit that depends on the car,
not from a fixed speed. If it does not, find out why.

### 2.4 A pit lane

A branch off the main circuit, entered by driving into an `Area3D`, with a speed
limit and a stop box. Stopping for two seconds restores something — tyre grip
that degrades over a race, say.

*Files:* level data (a second waypoint ring, or a lane offset), `race_director.gd`,
`hud.gd`, `ai_driver.gd` if the AI should ever pit.

*How to tell:* the lap counter must not break. This is the exercise that finds
out whether chapter 10's arc-length progress really is robust — a car in the
pits is off the racing line for several seconds and must neither gain nor lose a
lap.

---

## Tier 3 — a subsystem

### 3.1 Replays

Record the whole race, play it back with a free camera.

Two designs, and choosing between them is the exercise:

- **Record state**: every car's position, yaw and a few flags per tick. Large,
  robust, and plays back identically on any machine.
- **Record inputs**: every car's `CarInput` per tick, and re-simulate. Tiny —
  four floats per car per tick — and only correct if the simulation is
  deterministic, which chapter 15 says it is on one machine and is not across
  platforms.

*How to tell:* implement input recording and check whether the replay diverges.
Then measure *how long* it takes to diverge. That number is a direct measurement
of your simulation's determinism, and it is the number you would need before
attempting 3.4.

### 3.2 Split screen

Two players, two viewports, two cameras, one race.

Godot makes the rendering part easy — two `SubViewport`s with a `Camera3D`
each — and the interesting problems are elsewhere: `Input` actions are global,
so you need per-device action sets or explicit device filtering; the HUD has to
exist twice; chapter 10's `player_index` becomes a list.

*How to tell:* find everything that assumed one player. If chapter 01's advice
about not making `Player` an autoload was followed, this is an afternoon. If it
was not, it is a week — which is the lesson.

### 3.3 A different game on the same engine

Everything under `scripts/engine/` is game-agnostic: a spline with arc-length
queries, a collision grid, a prop batcher, a terrain fitter, a procedural
synthesiser.

Build something else with them. A **rally stage** (point-to-point, one car,
split times) is the smallest step. A **top-down shooter** on the same collision
and batching is a bigger one, and will find every place where "game-agnostic" was
optimistic.

*How to tell:* count the files you had to modify under `scripts/engine/`. Zero
is the target; every one you touched is a leak in the boundary chapter 01 drew,
and worth writing down.

### 3.4 Networked multiplayer

Godot has a high-level multiplayer API — `MultiplayerSpawner`,
`MultiplayerSynchronizer`, `@rpc` — which will get you a playable four-player
race in a weekend, with client-side positions interpolated from the host.

That is the easy version, and it is the right one to build first. The hard
version is **deterministic lockstep**: every peer simulates every car from
inputs alone, and only inputs cross the network. It needs the simulation to be
bit-identical everywhere, which chapter 15 says it is not, which means fixed
point or a very careful float discipline — and it is where chapter 16's C
kernels become load-bearing rather than an optimisation.

*How to tell:* for the easy version, race four peers and look for rubber-banding
under 100 ms of simulated latency. For the hard version, checksum every car's
position every 60 ticks and compare across peers; the first mismatch tells you
which subsystem is not deterministic.

### 3.5 An in-editor circuit designer

Chapter 05's pipeline B, taken seriously: an `EditorPlugin` with a custom
gizmo for the racing line, buttons to place gates and spawns, live validation
using chapter 15's content invariants, and a bake button.

*How to tell:* build a complete new circuit with it, without opening Blender or
a text editor, and have it pass every invariant test.

This is the most useful project on the list for an actual game. A tool that
makes bad levels impossible is worth more than a feature.

---

## Tier 4 — research projects

### 4.1 Make the AI faster than the player

Chapter 09's AI drives a naive line: centre-line plus a bend-proportional
offset. A real racing line is out-in-out, and finding the optimal one is a
constrained optimisation problem over the whole circuit.

Implement one: discretise the track into `N` stations with `M` lateral positions
each, define the cost of a path segment from the curvature it implies and the
speed the vehicle model permits, and solve with dynamic programming. Bake the
result into the level.

*How to tell:* the optimised AI's best lap versus the current AI's, and versus
yours. Then the honest question: is it more fun to race against? Chapter 09's
last exercise argues that a fast AI that never makes mistakes is *less* fun than
a slower one that does.

### 4.2 The terrain, on the GPU

Chapter 13 describes the displacement-shader version and ships the CPU one.
Build the GPU version properly: heightfield in a texture, normals derived in the
shader or packed into the texture's other channels, LOD from mesh subdivision,
and the relief shading from chapter 12 applied to both.

*How to tell:* build time, memory, and a screenshot comparison under low sun.
Then run it on the Compatibility renderer and see whether vertex texture fetch
behaves the way you expected.

### 4.3 Procedural circuits

Generate a circuit: a closed loop with plausible corner radii, elevation that
does not self-intersect, a width that varies sensibly, gravel traps on the
outside of fast corners, and scenery.

The generator has to satisfy every invariant in chapter 15's content tests —
which is the point. Those tests were written to catch human mistakes and they
turn out to be an excellent specification for a generator.

*How to tell:* generate a hundred circuits, run the invariant tests on all of
them, and count the failures. Then drive five and judge whether they are *good*,
which no test can tell you.

### 4.4 Port the simulation entirely to C

Chapter 16 moves two functions. Move all of it: spline, collision, vehicle
model, AI, race rules — the whole of `scripts/game/` and half of
`scripts/engine/` — behind one GDExtension, with GDScript reduced to
presentation and input.

*How to tell:* three measurements. Frame time versus the GDScript version.
Whether chapter 15's tier-2 tests can now run thousands of times faster than
real time. And how long it takes you to make a small handling change and see it
on screen — which is the cost you are paying, and the reason chapter 16 says not
to do this.

The interesting outcome is that the C files you end up with will look very like
`game/src/car.c` and `game/src/race.c` in this repository, because they are the
same program. Diff them. Where they differ, one of the two is wrong.

---

## How to work on this project

Six habits, all of which the earlier chapters demonstrate rather than state.

**Change one number at a time.** The tuning in `car_tuning.gd` is a system with
interactions. Changing `grip_tarmac` and `max_steer_top_speed` together and
finding the car better tells you nothing about which.

**Write the test that encodes the requirement, not the behaviour.** Chapter 15's
gravel assertion — "full throttle settles under 1.1 u/s" — came before the
tuning that satisfies it.

**Keep the boundary.** Nothing in `scripts/game/` touches a `Camera3D`,
a `Viewport` or a `MeshInstance3D`. When you find yourself wanting to, ask what
signal would carry the fact instead.

**Type everything, and let the parser enforce it.** Chapter 02's warning
settings are not a style preference; they are the difference between an error in
the editor and a bug on lap 3.

**Profile before optimising, and count before profiling.** Chapter 16's table
eliminates two candidates by arithmetic alone.

**Leave the reason, not the change.** The comments in this project that earn
their keep say *why* — why 0.06 units of sink, why `move_toward` and not `lerp`,
why the coarse sweep exists. A comment restating the code is noise; a comment
recording a decision is the only place that decision exists.

---

## Where to go next

**The C series.** [`docs/learn`](../learn/README.md) builds the same game
without an engine: an arena allocator, a JSON parser, SAT collision, a shadow
mapper, a Makefile. Reading it after this one is the more useful order, because
you now know which of those problems an engine solves and which it does not.

**Godot's own source.** `servers/rendering/` and `scene/3d/` are readable, and
after chapters 11 and 12 you will know what you are looking at. Reading
`CharacterBody3D::move_and_slide` is a good afternoon.

**A vehicle dynamics text**, if chapter 08 left you wanting the real model.
Pacejka's magic formula, load transfer, and the difference between what a
simulator and an arcade game are trying to achieve.

**Ship something.** The most valuable thing in this series is not any single
technique. It is that a complete game is a small number of subsystems, each of
which fits in a chapter, and that the difference between a demo and a game is
mostly the tuning, the tests and the tools.

---

Back to the [index](README.md).
