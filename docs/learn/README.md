# Learning from this codebase

A guided tour of a small 3D game engine written in C11 on top of
[raylib](https://www.raylib.com/), and of the top-down racer built with it.

The [top-level README](../../README.md) tells you how to *use* the project. This
series is about how it *works* and, more importantly, **why each piece is the
shape it is**. Every chapter takes one subsystem, explains the idea from first
principles, walks the real code line by line, and ends with exercises that
change the running game.

Nothing here is pseudocode. Every excerpt is copied from a file you can open,
edit and rebuild in under thirty seconds.

---

## Who this is for

You should be comfortable reading C: pointers, structs, `static`, the
preprocessor. You do **not** need to know graphics programming, linear algebra
beyond dot products, or raylib. Each chapter introduces the maths it needs.

If you have never built the project, do that first — the chapters assume you can
run `make test` and see output.

```sh
git submodule update --init
make            # builds raylib, then build/desktop/racer
make test       # 348 checks, no window needed
make run        # play
```

---

## The chapters

| # | Chapter | What you learn |
|---|---|---|
| 01 | [Architecture](01-architecture.md) | Engine/game layering, data flow, the frame loop, why C |
| 02 | [Memory: arena allocation](02-memory-arenas.md) | Bump allocators, lifetimes, alignment, allocation failure as a test |
| 03 | [Writing a JSON parser](03-json-parser.md) | Recursive descent, UTF-8, surrogate pairs, error reporting, hostile input |
| 04 | [Level format and the Blender pipeline](04-level-pipeline.md) | Data-driven content, coordinate spaces, exporters, tolerant loaders |
| 05 | [Splines and arc length](05-splines.md) | Catmull-Rom, centripetal parameterisation, nearest-point queries, hints |
| 06 | [Collision detection](06-collision.md) | Separating axis theorem, minimum translation vectors, uniform grids, CSR |
| 07 | [Vehicle physics](07-vehicle-physics.md) | Bicycle model, grip as exponential decay, fixed timesteps, drift, gradients |
| 08 | [The AI driver](08-ai-driver.md) | Pure pursuit, curvature probing, apex selection, physical speed limits |
| 09 | [Race rules and state](09-race-rules.md) | Arc-length progress, ordered gates, standings, rescue logic |
| 10 | [Rendering: batching and culling](10-rendering.md) | Static batching, frustum planes, per-draw light selection, chase cameras |
| 11 | [Shaders and shadow mapping](11-shaders-and-shadows.md) | Forward lighting, GLSL 100 vs 330, depth maps, bias, PCF, texel snapping |
| 12 | [Terrain from a racing line](12-terrain.md) | Inverse-distance weighting, heightfields, normals, cartographic relief |
| 13 | [Procedural audio](13-audio.md) | Synthesis in a callback, cross-thread state, oscillators and noise |
| 14 | [Testing a game without a window](14-testing.md) | Headless simulation, invariants over content, static shader validation |
| 15 | [Build system and portability](15-build-and-portability.md) | Makefiles, submodules, desktop vs DRM/KMS, Windows and MSYS2 |
| 16 | [Exercises](16-exercises.md) | Graded projects, from a one-line tweak to a new subsystem |

Read them in order the first time. After that they stand alone.

---

## How to read a chapter

Each one follows the same shape:

1. **The problem** — what would go wrong without this subsystem.
2. **The idea** — the technique, explained on its own.
3. **The code** — the real implementation, annotated.
4. **The tradeoffs** — what this design costs, and what the alternatives were.
5. **Exercises** — things to change, with a way to tell if you got it right.

Code excerpts are labelled with their source, like this:

```c
// engine/src/arena.c
void *ArenaAlloc(Arena *a, size_t size)
```

Open the file alongside. The excerpts are trimmed for focus; the file is the
truth.

---

## The shape of the codebase

```
engine/          reusable, game-agnostic, knows nothing about racing
  arena.*        bump allocator
  json.*         dependency-free JSON reader
  level.*        level file -> props, colliders, traps, spawns, waypoints
  spline.*       closed centre line: arc length, nearest point, width, gradient
  collide.*      oriented boxes on XZ, SAT, uniform-grid broadphase
  light.*        point and spot lights, per-draw relevance selection
  terrain.*      heightfield ground fitted to the racing line
  render.*       static batching, frustum culling, lighting, shadows, camera
  assets.*       name-keyed model/texture/sound cache
  audio.*        procedurally synthesised engine note, tyre scrub, beeps
  input.*        keyboard + gamepad mapped onto actions
  core.*         window, subsystems, fixed-timestep helper

game/            the racer itself, knows nothing about windows
  car.*          arcade vehicle physics with drift
  ai.*           racing-line follower with corner speed and car avoidance
  race.*         grid, laps, checkpoints, standings, collision resolution
  skid.*         rubber laid under braking and sliding
  hud.*          readouts, minimap, countdown, results
  main.c         wiring and the frame loop

tools/blender/   the level editor add-on and the demo-track generator
tests/           headless engine + full-race simulation tests
levels/          the two shipping circuits, JSON + .blend sources
vendor/raylib/   raylib 5.5, a git submodule, built by the Makefile
```

Roughly 9,500 lines: 3,400 of engine source plus 760 of engine headers, 1,940 of
game source plus 350 of game headers, 950 of tests, and 2,160 of Python tooling.
Small enough to read in a weekend, complete enough to be a real game.

---

## A note on scale

The whole project is built around one arbitrary decision: **one world unit is
one road tile from the Kenney kit**. A car is 0.30 × 0.60 units. The drivable
lane is 0.69 wide. Top speed is 6.6 units per second.

Every number you meet — grip of 9.5, gravity of 11.0, a shadow box 34 units
across, a terrain cell of 0.4 — is in that scale. When a chapter says a value
"is about a seventh of racing pace", that is what it means. Nothing is in metres
and nothing is in metres per second squared; trying to read it that way will
confuse you.
