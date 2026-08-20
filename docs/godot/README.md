# Building this racer in Godot

A guided tour of the same top-down racer — same circuits, same handling, same
race rules — rebuilt on **Godot 4** with **statically typed GDScript**, dropping
to **C** only where a measurement says GDScript is not fast enough.

The sibling series in [`docs/learn`](../learn/README.md) explains the C11 +
raylib engine that ships in this repository: every subsystem written by hand,
from the allocator up. This series answers a different question:

> Which of those subsystems does Godot already own, which ones do you still
> have to write, and what changes about the ones you keep?

The answer is not "Godot does everything". Roughly half the engine disappears
into the editor and the servers. The other half — the racing line, the AI, the
vehicle model, the race rules — is *the game*, and you still write all of it.
Knowing which half is which is the whole skill.

---

## Who this is for

You should be able to read GDScript and have opened the Godot editor at least
once. You do **not** need to have read the C series, though each chapter names
the one it mirrors so you can compare.

Everything here targets **Godot 4.3 or newer**, and says so explicitly where a
feature needs something later (typed dictionaries are 4.4+, `@abstract` is
4.5+). Nothing here uses C#.

---

## Two rules this series never breaks

**1. Every declaration is typed.** No bare `var`, no untyped parameters, no
untyped returns, no `Array` without an element type. Not as a style preference —
as a compiler setting, turned on in chapter 02, that fails the build otherwise.
Typed GDScript is a different language from dynamic GDScript: it type-checks at
parse time, it compiles to specialised opcodes that skip Variant dispatch, and
it makes the editor able to answer "what is this". Dynamic GDScript is a
prototyping mode this project does not use.

**2. C is a conclusion, not a starting point.** Nothing gets written in C
because it "sounds like it needs to be fast". It gets written in C after the
profiler names it, and chapter 16 does exactly that: profile the finished game,
find the three functions that actually matter, and move those — as plain C11,
behind the thinnest possible GDExtension wrapper — while everything else stays
in GDScript. Two of the three candidates you would guess turn out not to need
it.

---

## The chapters

| # | Chapter | What you learn | C-series chapter it mirrors |
|---|---|---|---|
| 01 | [Architecture](01-architecture.md) | Nodes, scenes, autoloads, and where to draw the line that keeps the simulation headless-testable | 01 |
| 02 | [Typed GDScript](02-typed-gdscript.md) | Annotations, inference, typed containers, the four warnings to promote to errors, what typing actually costs and buys | — |
| 03 | [Data and lifetime](03-data-and-lifetime.md) | `Node` vs `Resource` vs `RefCounted`, ownership, `queue_free`, packed arrays, pooling — what replaces the arena | 02 |
| 04 | [Level data without a parser](04-level-format.md) | Custom `Resource` types, `.tres`, `ResourceLoader`, and when JSON is still the right answer | 03, 04 |
| 05 | [The Blender pipeline](05-blender-pipeline.md) | glTF and `.blend` import, coordinate spaces, `@tool` scripts, authoring a circuit in the Godot editor instead | 04 |
| 06 | [Splines and arc length](06-splines.md) | `Curve3D`, baked points, `get_closest_offset`, and the resampled table you still need | 05 |
| 07 | [Collision](07-collision.md) | Godot physics vs the hand-written SAT, `CharacterBody3D`, direct space state, layers and masks | 06 |
| 08 | [Vehicle physics](08-vehicle-physics.md) | The bicycle model in typed GDScript, `_physics_process`, drift, gradients, interpolation | 07 |
| 09 | [The AI driver](09-ai-driver.md) | Pure pursuit, curvature probing, apex selection — and why `NavigationServer3D` is the wrong tool here | 08 |
| 10 | [Race rules and state](10-race-rules.md) | Arc-length progress, ordered gates, signals, standings, a state machine that survives a scene change | 09 |
| 11 | [Rendering](11-rendering.md) | `MultiMeshInstance3D`, what culling Godot does for you, LOD, occlusion, and the draw-call budget | 10 |
| 12 | [Shaders, lights and shadows](12-shaders-and-shadows.md) | The Godot shading language, `DirectionalLight3D` shadows, `WorldEnvironment`, the relief-shading trick | 11 |
| 13 | [Terrain from a racing line](13-terrain.md) | `ArrayMesh`, `SurfaceTool`, `HeightMapShape3D`, inverse-distance weighting, chunking | 12 |
| 14 | [Procedural audio](14-audio.md) | `AudioStreamGenerator`, synthesis without a callback thread, buses and effects | 13 |
| 15 | [Testing without a window](15-testing.md) | `--headless`, gdUnit4, driving a whole race from a script, determinism | 14 |
| 16 | [Writing the hot paths in C](16-native-c.md) | Profiling first, then GDExtension: plain C11 kernels behind a minimal binding, and the build for each platform | — |
| 17 | [Export and platforms](17-export-and-platforms.md) | Export presets, the three renderers, the Raspberry Pi, shipping the native library | 15 |
| 18 | [Exercises](18-exercises.md) | Graded projects, from a tuning tweak to a networked race | 16 |

Read them in order the first time. After that they stand alone.

---

## How to read a chapter

Same shape as the C series:

1. **The problem** — what would go wrong without this.
2. **What Godot gives you** — the engine feature, honestly scoped.
3. **What you still write** — the real code, typed and annotated.
4. **The tradeoffs** — what this design costs.
5. **Exercises** — things to change, with a way to tell if you got it right.

Code excerpts are labelled with their source, like this:

```gdscript
# scripts/game/car_body.gd
func _physics_process(delta: float) -> void:
```

---

## The shape of the project

```
godot/
  project.godot
  scenes/
    main.tscn                 root: world, HUD, race director
    car.tscn                  CharacterBody3D + mesh + skid emitters
    circuit.tscn              instanced per level: props, path, lights
  scripts/
    engine/                   game-agnostic. Knows nothing about racing.
      track_spline.gd         resampled centre line: arc length, closest point
      collision_builder.gd    level colliders -> one StaticBody3D of shapes
      collision_grid.gd       optional hand-written broadphase (chapter 07)
      prop_batcher.gd         level props -> MultiMeshInstance3D per model
      terrain_builder.gd      heightfield ArrayMesh fitted to the racing line
      procedural_audio.gd     engine note, tyre scrub, countdown blips
      fixed_step.gd           helpers around _physics_process
    game/                     the racer. Knows nothing about windows.
      car_tuning.gd           Resource: every handling number
      car_body.gd             one tick of a vehicle
      ai_driver.gd            RefCounted: racing-line follower
      race_director.gd        grid, laps, gates, standings
      skid_trails.gd          rubber laid under braking and sliding
      hud.gd                  readouts, minimap, countdown, results
    data/
      level_data.gd           Resource: the whole circuit as data
  levels/
    circuit01.tres            Ardennes
    circuit02.tres            Eifel
  native/                     chapter 16 only
    src/*.c                   plain C11 kernels, no Godot headers
    src/register_types.cpp    ~120 lines of godot-cpp glue
    racer.gdextension
  tests/                      gdUnit4 suites, run with --headless
  assets/models/*.glb         the Kenney kit, unchanged
```

Everything under `scripts/engine/` is a `Node` or `Resource` that never touches
the race; everything under `scripts/game/` never touches a `Viewport`. That is
the same one hard boundary the C engine draws, for the same reason: chapter 15
runs a complete six-car race with `--headless` and no rendering at all.

---

## A note on scale

Unchanged from the C version, and worth restating because every number in these
chapters is in it:

**One world unit is one road tile from the Kenney kit.** A car is 0.30 × 0.60
units. The drivable lane is 0.69 wide. Top speed is 6.6 units per second. Grip
is a decay rate of 9.5 per second. Gravity along a slope is 11.0.

Nothing is in metres. This matters more in Godot than it did in raylib, because
Godot's physics defaults — gravity of 9.8, a default `CharacterBody3D` step
height, the 0.001 default `safe_margin` — are all written for a metres-and-people
scale. A 0.30-unit-wide car is a small object by those defaults, and chapter 07
covers what has to change because of it.

---

Start with [01 — Architecture](01-architecture.md).
